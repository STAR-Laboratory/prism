#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/plugin.h"
#include "dram_controller/impl/plugin/prac/prac.h"
#include "dram_controller/impl/plugin/device_config/device_config.h"

#include <limits>
#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cassert>
#include <cinttypes>

namespace Ramulator {

class MOPAC : public IControllerPlugin, public Implementation, public IPRAC {
    RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, MOPAC, "MOPAC-D",
        "MoPAC-D")

private:
    class PerBankCounters;

    // Time budget for SRQ drain inside one ABO is 350ns; each RMW takes ~70ns,
    // so up to 5 SRQ entries can be drained per ABO (MoPAC paper, Sec 6.1).
    static constexpr int kAboDrainMax = 5;

private:
    DeviceConfig m_cfg;
    std::vector<MOPAC::PerBankCounters> m_bank_counters;
    std::vector<int> m_same_bank_offsets;

    Clk_t m_clk = 0;

    ABOState m_state = ABOState::NORMAL;
    Clk_t m_abo_recovery_start = std::numeric_limits<Clk_t>::max();

    int m_abo_act_ns = -1;
    int m_abo_recovery_refs = -1;
    int m_abo_delay_acts = -1;
    int m_abo_thresh = -1;

    int m_abo_act_cycles = -1;

    uint32_t m_abo_recov_rem_refs = -1;
    uint32_t m_abo_delay_rem_acts = -1;
    bool m_is_abo_needed = false;

    bool m_debug = false;

    uint32_t m_psq_size = 0;
    uint32_t m_insertion_th = 0;


    bool m_enable_prac_reset_at_refresh = false;

    // MoPAC-D configurations.
    double   m_prob = -1;                       // p; SRQ insertion probability.
    uint32_t m_tardiness_th = 32;               // TTH (Tardiness Threshold); paper default.
    int      m_num_drain = -1;                  // SRQ drain count per REF.
    uint32_t m_srq_size = 0;                    // Per-chip SRQ capacity.

    // Per-chip independent randomness (Appendix-B of MoPAC paper).
    uint32_t m_num_chips = 8;                   // x8 default; configurable.

    // Mitigations performed under retention refresh (JESD79-5C.01).
    uint32_t m_targeted_ref_frequency = 0;

    bool m_enable_opportunistic_mitigation = true;

    // Stats
    uint64_t s_num_recovery = 0;                // # of ABOs.
    uint64_t s_num_targeted_ref = 0;

    uint64_t s_psq_len = 0;
    double   s_avg_psq_len = 0.0;

    uint64_t s_srq_len = 0;                     // Total SRQ entries (across chips & banks).
    double   s_avg_srq_len = 0.0;

    static uint64_t s_num_acts;
    static uint64_t s_num_srq_update;
    static uint64_t s_num_remained_critical_rows;

    static uint64_t s_num_case_one;
    static uint64_t s_num_case_two;
    static uint64_t s_num_case_three;
    static double   s_avg_srq_update;

    // Cached command IDs - avoid string lookup in the hot path.
    int m_cmd_prea  = -1;
    int m_cmd_rfmab = -1;
    int m_cmd_rfmsb = -1;
    int m_cmd_act   = -1;

    // Alert-cause invariant checking (debug / sanity).
    bool     m_enable_alert_cycle_check = true;
    uint32_t m_stats_sample_period      = 1024;

    uint64_t s_num_alert_case1_only        = 0;
    uint64_t s_num_alert_case2_only        = 0;
    uint64_t s_num_alert_case3_only        = 0;
    uint64_t s_num_alert_multiple_cases    = 0;
    uint64_t s_num_alert_no_reason         = 0;

    uint64_t s_num_alert_latch_set_by_cycle_check     = 0;
    uint64_t s_num_alert_latch_cleared_by_cycle_check = 0;

    uint64_t s_num_delay_exit_with_case1_pending = 0;
    uint64_t s_num_delay_exit_with_case2_pending = 0;
    uint64_t s_num_delay_exit_with_case3_pending = 0;

    uint64_t s_num_case1_true_but_srq_not_full = 0;
    uint64_t s_num_case2_true_but_no_moat_ath  = 0;
    uint64_t s_num_case3_true_but_no_tth       = 0;

    // MINT pick histogram (aggregated across banks).
    std::vector<uint64_t> s_mint_pick_hist;
    std::vector<double>   s_mint_pick_pct;
    uint64_t s_mint_total_picks = 0;
    double   s_mint_chi_square  = 0.0;
    uint32_t m_mint_W_global    = 0;

    // Mitigation accounting.
    uint64_t s_num_total_mitigations          = 0;
    uint64_t s_num_prac_counter_resets        = 0;   // MOAT refresh-time resets (rows).

public:
    void init() override {
        m_debug             = param<bool>("debug").default_val(false);
        m_abo_delay_acts    = param<int>("abo_delay_acts").default_val(1);
        m_abo_recovery_refs = param<int>("abo_recovery_refs").default_val(1);  // 1 RFM/ABO.
        m_abo_act_ns        = param<int>("abo_act_ns").default_val(180);
        m_abo_thresh        = param<int>("abo_threshold").default_val(60);

        m_psq_size     = param<uint32_t>("psq_size").default_val(1);
        m_insertion_th = param<uint32_t>("insertion_th").default_val(1);

        m_tardiness_th = param<uint32_t>("tardiness_th").default_val(32);

        m_enable_prac_reset_at_refresh =
              param<bool>("enable_prac_reset_at_refresh").default_val(false);

        // Per-chip independent randomness (Appendix-B). Default to x8 device.
        m_num_chips = param<uint32_t>("num_chips").default_val(8);
        if (m_num_chips == 0) {
            std::printf("[MOPAC] Warning: num_chips=0 is invalid; clamping to 1.\n");
            m_num_chips = 1;
        }

        // MoPAC-D-specific.
        m_prob      = param<double>("mitigation_prob").default_val(1.0 / 8.0);
        m_num_drain = param<int>("num_drain").default_val(2);      // p=1/8: 2 drains/REF.

        // Per-chip SRQ capacity. MoPAC paper default is 16 entries per chip.
        m_srq_size  = param<uint32_t>("srq_size").default_val(16);

        // Drain-on-REF (Targeted Refresh).
        m_targeted_ref_frequency = param<uint32_t>("targeted_ref_frequency").default_val(1);

        m_enable_opportunistic_mitigation = param<bool>("enable_opportunistic_mitigation").default_val(true);

        m_enable_alert_cycle_check = param<bool>("enable_alert_cycle_check").default_val(true);
        m_stats_sample_period      = param<uint32_t>("stats_sample_period").default_val(1024);
    }

    void setup(IFrontEnd* /*frontend*/, IMemorySystem* /*memory_system*/) override {
        m_cfg.set_device(cast_parent<IDRAMController>());
        init_dram_params(m_cfg.m_dram);

        // Cache command IDs once for the hot path.
        m_cmd_prea  = m_cfg.m_dram->m_commands("PREA");
        m_cmd_rfmab = m_cfg.m_dram->m_commands("RFMab");
        m_cmd_rfmsb = m_cfg.m_dram->m_commands("RFMsb");
        m_cmd_act   = m_cfg.m_dram->m_commands("ACT");

        m_is_abo_needed   = false;
        m_abo_act_cycles  = m_abo_act_ns /
            (static_cast<float>(m_cfg.m_dram->m_timing_vals("tCK_ps")) / 1000.0f);

        // Register stats.
        register_stat(s_num_targeted_ref).name("num_targeted_ref");
        register_stat(s_num_total_mitigations).name("num_total_mitigations");
        register_stat(s_num_prac_counter_resets).name("num_prac_counter_resets");

        register_stat(s_psq_len).name("psq_len");
        register_stat(s_avg_psq_len).name("avg_psq_len");

        register_stat(s_srq_len).name("srq_len");
        register_stat(s_avg_srq_len).name("avg_srq_len");

        register_stat(s_num_acts).name("num_acts");
        register_stat(s_num_srq_update).name("num_srq_update");
        register_stat(s_avg_srq_update).name("avg_srq_update");
        register_stat(s_num_remained_critical_rows).name("num_remained_critical_rows");

        register_stat(s_num_case_one).name("num_case_one");
        register_stat(s_num_case_two).name("num_case_two");
        register_stat(s_num_case_three).name("num_case_three");

        register_stat(s_num_alert_case1_only).name("num_alert_case1_only");
        register_stat(s_num_alert_case2_only).name("num_alert_case2_only");
        register_stat(s_num_alert_case3_only).name("num_alert_case3_only");
        register_stat(s_num_alert_multiple_cases).name("num_alert_multiple_cases");
        register_stat(s_num_alert_no_reason).name("num_alert_no_reason");

        register_stat(s_num_alert_latch_set_by_cycle_check).name("num_alert_latch_set_by_cycle_check");
        register_stat(s_num_alert_latch_cleared_by_cycle_check).name("num_alert_latch_cleared_by_cycle_check");

        register_stat(s_num_delay_exit_with_case1_pending).name("num_delay_exit_with_case1_pending");
        register_stat(s_num_delay_exit_with_case2_pending).name("num_delay_exit_with_case2_pending");
        register_stat(s_num_delay_exit_with_case3_pending).name("num_delay_exit_with_case3_pending");

        register_stat(s_num_case1_true_but_srq_not_full).name("num_case1_true_but_srq_not_full");
        register_stat(s_num_case2_true_but_no_moat_ath).name("num_case2_true_but_no_moat_ath");
        register_stat(s_num_case3_true_but_no_tth).name("num_case3_true_but_no_tth");

        // Construct per-bank counters.
        m_bank_counters.reserve(m_cfg.m_num_banks);
        for (int i = 0; i < m_cfg.m_num_banks; i++) {
            m_bank_counters.emplace_back(
                i, m_cfg,
                m_is_abo_needed,
                m_abo_thresh,
                m_debug,
                m_psq_size, m_insertion_th, m_tardiness_th,
                m_targeted_ref_frequency,
                m_enable_opportunistic_mitigation,
                s_num_total_mitigations, s_num_targeted_ref,
                m_prob, m_num_drain, m_srq_size,
                m_num_chips,
                m_enable_prac_reset_at_refresh,
                s_num_prac_counter_resets,
                m_cmd_act, m_cmd_rfmab, m_cmd_rfmsb);
        }

        register_stat(s_num_recovery).name("prac_num_recovery");

        // Configure MINT-pick histogram.
        m_mint_W_global = static_cast<uint32_t>(std::llround(1.0 / m_prob));
        if (m_mint_W_global == 0) m_mint_W_global = 1;

        s_mint_pick_hist.assign(m_mint_W_global + 1, 0);
        s_mint_pick_pct.assign(m_mint_W_global + 1, 0.0);

        register_stat(s_mint_total_picks).name("mint_total_picks");
        register_stat(s_mint_chi_square).name("mint_pick_chi_square");
        for (uint32_t i = 1; i <= m_mint_W_global; i++) {
            register_stat(s_mint_pick_hist[i]).name("mint_pick_" + std::to_string(i) + "_count");
            register_stat(s_mint_pick_pct[i]).name("mint_pick_" + std::to_string(i) + "_pct");
        }

        std::printf("[MOPAC][SETUP] abo_threshold=%d, p=1/%u, num_chips=%u, "
                    "srq_size=%u (per-chip), TTH=%u, "
                    "PSQ size=%u, insertion_th=%u, "
                    "prac_reset_at_refresh=%s\n",
                    m_abo_thresh, m_mint_W_global, m_num_chips, m_srq_size,
                    m_tardiness_th,
                    m_psq_size, m_insertion_th,
                    m_enable_prac_reset_at_refresh ? "ON" : "OFF");
    }

    void update(bool request_found, ReqBuffer::iterator& req_it) override {
        m_clk++;

        const Request* req_ptr = request_found ? &(*req_it) : nullptr;
        update_state_machine(request_found, req_ptr);

        // Lightweight per-cycle stats accumulation.
        for (size_t i = 0; i < m_cfg.m_num_banks; i++) {
            s_psq_len += m_bank_counters[i].get_psq_size();
            s_srq_len += m_bank_counters[i].get_total_srq_size();
        }

        if (!request_found) {
            return;
        }

        auto& req       = *req_it;
        auto& req_meta  = m_cfg.m_dram->m_command_meta(req.command);
        auto& req_scope = m_cfg.m_dram->m_command_scopes(req.command);

        bool has_bank_wildcard      = req.addr_vec[m_cfg.m_bank_level]      == -1;
        bool has_bankgroup_wildcard = req.addr_vec[m_cfg.m_bankgroup_level] == -1;

        if (has_bankgroup_wildcard && has_bank_wildcard) {       // All BG, All Bank.
            int offset = req.addr_vec[m_cfg.m_rank_level] * m_cfg.m_num_banks_per_rank;
            for (int i = 0; i < m_cfg.m_num_banks_per_rank; i++) {
                m_bank_counters[offset + i].on_request(req);
            }
            req.addr_vec[m_cfg.m_bank_level] = -1;
        } else if (has_bankgroup_wildcard) {                     // All BG, Single Bank.
            int rank_offset = req.addr_vec[m_cfg.m_rank_level] * m_cfg.m_num_banks_per_rank;
            int bank_offset = req.addr_vec[m_cfg.m_bank_level];
            for (int i = 0; i < m_cfg.m_num_bankgroups; i++) {
                int bg_offset = i * m_cfg.m_num_banks_per_bankgroup;
                m_bank_counters[rank_offset + bg_offset + bank_offset].on_request(req);
            }
        } else if (has_bank_wildcard) {                          // Single BG, All Bank.
            int rank_offset = req.addr_vec[m_cfg.m_rank_level] * m_cfg.m_num_banks_per_rank;
            int bg_offset   = req.addr_vec[m_cfg.m_bankgroup_level] * m_cfg.m_num_banks_per_bankgroup;
            for (int i = 0; i < m_cfg.m_num_banks_per_bankgroup; i++) {
                m_bank_counters[rank_offset + bg_offset + i].on_request(req);
            }
        } else {                                                  // Single BG, Single Bank.
            m_bank_counters[m_cfg.get_flat_bank_id(req)].on_request(req);
        }
    }

    // -----------------------------------------------------------------------
    // ABO state-machine plumbing
    // -----------------------------------------------------------------------

    struct AlertSummary {
        bool c1 = false;  // Some chip's SRQ is full.
        bool c2 = false;  // MOAT row >= ATH*.
        bool c3 = false;  // Some chip's SRQ has ACtr >= TTH.

        uint32_t banks_c1 = 0;
        uint32_t banks_c2 = 0;
        uint32_t banks_c3 = 0;

        bool any() const { return c1 || c2 || c3; }
    };

    AlertSummary refresh_all_alert_reasons_and_check() {
        AlertSummary s;

        for (auto& bc : m_bank_counters) {
            bc.refresh_alert_reasons();

            if (bc.pending_case_one())   { s.c1 = true; s.banks_c1++; }
            if (bc.pending_case_two())   { s.c2 = true; s.banks_c2++; }
            if (bc.pending_case_three()) { s.c3 = true; s.banks_c3++; }

            // Invariant checks: pending flags should agree with the actual state.
            if (bc.pending_case_one()   && !bc.actual_case_one())   s_num_case1_true_but_srq_not_full++;
            if (bc.pending_case_two()   && !bc.actual_case_two())   s_num_case2_true_but_no_moat_ath++;
            if (bc.pending_case_three() && !bc.actual_case_three()) s_num_case3_true_but_no_tth++;
        }

        return s;
    }

    void count_alert_cause(const AlertSummary& s) {
        const int n = (s.c1 ? 1 : 0) + (s.c2 ? 1 : 0) + (s.c3 ? 1 : 0);
        if (n == 0)               s_num_alert_no_reason++;
        else if (n == 1 && s.c1)  s_num_alert_case1_only++;
        else if (n == 1 && s.c2)  s_num_alert_case2_only++;
        else if (n == 1 && s.c3)  s_num_alert_case3_only++;
        else                       s_num_alert_multiple_cases++;
    }

    static const char* state_name(ABOState s) {
        switch (s) {
            case ABOState::NORMAL:       return "NORMAL";
            case ABOState::PRE_RECOVERY: return "PRE_RECOVERY";
            case ABOState::RECOVERY:     return "RECOVERY";
            case ABOState::DELAY:        return "DELAY";
            default:                     return "UNKNOWN";
        }
    }

    void update_state_machine(bool request_found, const Request* req) {
        const auto cur_state = m_state;

        AlertSummary reasons;
        if (m_enable_alert_cycle_check) {
            reasons = refresh_all_alert_reasons_and_check();
        }

        switch (m_state) {
        case ABOState::NORMAL: {
            if (m_enable_alert_cycle_check) {
                const bool old_needed = m_is_abo_needed;
                m_is_abo_needed = reasons.any();
                if (!old_needed && m_is_abo_needed)  s_num_alert_latch_set_by_cycle_check++;
                if ( old_needed && !m_is_abo_needed) s_num_alert_latch_cleared_by_cycle_check++;
            }

            if (m_is_abo_needed) {
                if (m_enable_alert_cycle_check) count_alert_cause(reasons);
                if (m_debug) {
                    std::printf("[PRAC] [%lu] <%s> Asserting ALERT_N.\n",
                                m_clk, state_name(cur_state));
                }
                m_state = ABOState::PRE_RECOVERY;
                m_abo_recovery_start = m_clk + m_abo_act_cycles;
                s_num_recovery++;
            }
            break;
        }

        case ABOState::PRE_RECOVERY:
            if (m_debug && request_found && req && req->command == m_cmd_prea) {
                std::printf("[PRAC] [%lu] <%s> Received PREA.\n",
                            m_clk, state_name(cur_state));
            }
            if (m_clk == m_abo_recovery_start) {
                m_state = ABOState::RECOVERY;
                m_abo_recovery_start = std::numeric_limits<Clk_t>::max();
                m_abo_recov_rem_refs = m_abo_recovery_refs * m_cfg.m_num_ranks;
            }
            break;

        case ABOState::RECOVERY:
            if (request_found && req &&
                (req->command == m_cmd_rfmab || req->command == m_cmd_rfmsb)) {
                m_abo_recov_rem_refs--;
                if (!m_abo_recov_rem_refs) {
                    m_state = ABOState::DELAY;
                    m_abo_delay_rem_acts = m_abo_delay_acts;
                }
            }
            break;

        case ABOState::DELAY:
            if (request_found && req && req->command == m_cmd_act) {
                m_abo_delay_rem_acts--;
                if (!m_abo_delay_rem_acts) {
                    AlertSummary exit_reasons = m_enable_alert_cycle_check
                        ? refresh_all_alert_reasons_and_check()
                        : AlertSummary{};

                    if (exit_reasons.c1) s_num_delay_exit_with_case1_pending++;
                    if (exit_reasons.c2) s_num_delay_exit_with_case2_pending++;
                    if (exit_reasons.c3) s_num_delay_exit_with_case3_pending++;

                    m_is_abo_needed = m_enable_alert_cycle_check
                        ? exit_reasons.any()
                        : false;

                    if (!m_enable_alert_cycle_check) {
                        for (int i = 0; i < m_cfg.m_num_banks; i++) {
                            m_is_abo_needed |= m_bank_counters[i].is_critical();
                        }
                    }

                    m_state = ABOState::NORMAL;
                }
            }
            break;
        }

        if (m_debug && cur_state != m_state) {
            std::printf("[PRAC] [%lu] <%s> -> <%s>\n",
                        m_clk, state_name(cur_state), state_name(m_state));
        }
    }

    Clk_t next_recovery_cycle() override { return m_abo_recovery_start; }
    int   get_num_abo_recovery_refs() override { return m_abo_recovery_refs; }
    ABOState get_state() override { return m_state; }

    void finalize() override {
        s_avg_psq_len = static_cast<double>(s_psq_len) /
                        static_cast<double>(m_clk) /
                        static_cast<double>(m_cfg.m_num_banks);

        // s_srq_len already sums across chips within each bank.
        // Average is over banks; per-chip averages = s_avg_srq_len / m_num_chips.
        s_avg_srq_len = static_cast<double>(s_srq_len) /
                        static_cast<double>(m_clk) /
                        static_cast<double>(m_cfg.m_num_banks);

        s_avg_srq_update = (s_num_acts > 0)
            ? static_cast<double>(s_num_srq_update) / static_cast<double>(s_num_acts)
            : 0.0;
    }

private:
    // -----------------------------------------------------------------------
    // PerBankCounters: state for a single bank, including per-chip SRQs.
    // -----------------------------------------------------------------------
    class PerBankCounters {
    public:
        PerBankCounters(int bank_id, DeviceConfig& cfg,
                        bool& is_abo_needed,
                        int alert_thresh, bool debug,
                        uint32_t psq_size, uint32_t insertion_th,
                        uint32_t tardiness_th,
                        uint32_t targeted_ref_frequency,
                        bool enable_opportunistic_mitigation,
                        uint64_t& num_total_mitigations,
                        uint64_t& num_targeted_ref,
                        double mitigation_prob,
                        int num_drain, uint32_t srq_size,
                        uint32_t num_chips,
                        bool enable_prac_reset_at_refresh,
                        uint64_t& num_prac_counter_resets,
                        int cmd_act, int cmd_rfmab, int cmd_rfmsb)
            : m_cfg(cfg),
              m_is_abo_needed(is_abo_needed),
              m_alert_thresh(alert_thresh),
              m_debug(debug),
              m_bank_id(bank_id),
              m_psq_size(psq_size),
              m_insertion_th(insertion_th),
              m_tardiness_th(tardiness_th),
              m_targeted_ref_frequency(targeted_ref_frequency),
              m_enable_opportunistic_mitigation(enable_opportunistic_mitigation),
              s_num_total_mitigations(num_total_mitigations),
              s_num_targeted_ref(num_targeted_ref),
              m_prob(mitigation_prob),
              m_num_drain(num_drain),
              m_srq_size(srq_size),
              m_num_chips(num_chips),
              m_enable_prac_reset_at_refresh(enable_prac_reset_at_refresh),
              s_num_prac_counter_resets(num_prac_counter_resets),
              m_cmd_act(cmd_act),
              m_cmd_rfmab(cmd_rfmab),
              m_cmd_rfmsb(cmd_rfmsb),
              prob_dist(0.0, 1.0) {
            init_dram_params(m_cfg.m_dram);
            // MOAT counter-reset rate = num_rows / 8192 (REFs per tREFW).
            // For 128K-row banks → 16 rows/REFab.
            m_prac_reset_rows_per_ref =
                  std::max(1u, static_cast<uint32_t>(m_cfg.m_num_rows_per_bank) / 8192u);
            reset();
        }

        ~PerBankCounters() = default;

        // Hot path: switch on cached command IDs instead of std::function dispatch.
        void on_request(const Request& req) {
            const int cmd = req.command;
            if (cmd == m_cmd_act) {
                process_act(req);
            } else if (cmd == m_cmd_rfmab || cmd == m_cmd_rfmsb) {
                process_rfm(req);
            } else if (cmd == m_cmd_refab) {
                process_targeted_ref(req);
            }
            // Other commands (PRE, PREA, ...) are no-ops here.
        }

        void init_dram_params(IDRAM* dram) {
            // Cache REFab here (rest are passed in at construction).
            if (!dram->m_commands.contains("REFab")) {
                std::cout << "[MOPAC] Command REFab does not exist." << std::endl;
                std::exit(1);
            }
            m_cmd_refab = dram->m_commands("REFab");
        }

        void reset() {
            m_counters.clear();
            m_critical_rows.clear();
            m_psq.clear();

            m_chip_srqs.assign(m_num_chips, std::unordered_map<int, SRQEntry>{});
            for (auto& srq : m_chip_srqs) {
                srq.reserve(m_srq_size * 2);   // Avoid rehash thrash near capacity.
            }

            m_prac_reset_cursor = 0;

            mint_init();
        }

        bool is_critical() const { return !m_critical_rows.empty(); }

        // Total SRQ occupancy across all chips (for stats).
        uint64_t get_total_srq_size() const {
            uint64_t total = 0;
            for (const auto& srq : m_chip_srqs) total += srq.size();
            return total;
        }

        size_t get_psq_size() const { return m_psq.size(); }

        void refresh_alert_reasons() {
            refresh_local_abo_reasons();
        }

        bool pending_case_one()   const { return m_srq_abo_case_one;   }
        bool pending_case_two()   const { return m_srq_abo_case_two;   }
        bool pending_case_three() const { return m_srq_abo_case_three; }

        bool actual_case_one()   const { return is_any_chip_srq_full(); }
        bool actual_case_two()   const { return has_moat_ath_hit();     }
        bool actual_case_three() const { return has_tth_hit();          }

    private:
        // ---- Types -----------------------------------------------------------
        struct SRQEntry {
            uint32_t act_counter = 0;   // ACtr: incremented on every activation when in SRQ.
            uint32_t sc_counter  = 0;   // SCtr: incremented on each MINT (re-)selection.
        };

        struct MintSampler {
            uint32_t step          = 0;
            uint32_t pick          = 1;
            int      selected_row  = -1;
            uint64_t pick_seq      = 0;
            uint64_t window_seq    = 0;
            std::mt19937 gen;
            std::uniform_int_distribution<uint32_t> pick_dist;
        };

        // ---- Helpers ---------------------------------------------------------
        bool is_any_chip_srq_full() const {
            for (const auto& srq : m_chip_srqs) {
                if (srq.size() >= m_srq_size) return true;
            }
            return false;
        }

        bool has_tth_hit() const {
            if (m_tardiness_th == 0) return false;   // Disabled.
            for (const auto& srq : m_chip_srqs) {
                for (const auto& [row, entry] : srq) {
                    if (entry.act_counter >= m_tardiness_th) return true;
                }
            }
            return false;
        }

        bool has_moat_ath_hit() const {
            if (m_psq.empty()) return false;
            const auto moat = std::max_element(
                m_psq.begin(), m_psq.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
            return moat != m_psq.end() &&
                   moat->second >= ath_shared_scale();
        }

        // m_counters is the BANK-SHARED PRAC counter. Each chip's SRQ drain
        // independently bumps it by (1 + SCtr*W), so after N real ACTs the
        // shared counter is ~num_chips * (per-chip counter). To compare it
        // against per-chip thresholds (ATH, ETH/insertion_th) as written in
        // the MOAT/MoPAC papers, we scale the threshold up by num_chips.
        // This is a sum-vs-max approximation: it fires when avg(per-chip) >=
        // threshold, which for independently-sampled chips ~= max >= threshold.
        uint32_t ath_shared_scale() const {
            return static_cast<uint32_t>(m_alert_thresh) * m_num_chips;
        }
        uint32_t insertion_th_shared_scale() const {
            return m_insertion_th * m_num_chips;
        }

        // Locate the highest-counter row in PSQ (MOAT), if any.
        std::unordered_map<int, uint32_t>::iterator find_moat() {
            if (m_psq.empty()) return m_psq.end();
            return std::max_element(
                m_psq.begin(), m_psq.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });
        }

        void refresh_local_abo_reasons() {
            m_srq_abo_case_one   = is_any_chip_srq_full();
            m_srq_abo_case_two   = has_moat_ath_hit();
            m_srq_abo_case_three = has_tth_hit();

            if (m_srq_abo_case_two) {
                auto moat = find_moat();
                if (moat != m_psq.end()) {
                    m_critical_rows[moat->first] = m_counters[moat->first];
                    m_abo_row_addr = moat->first;
                }
            }
        }

        bool has_local_abo_reason() const {
            return m_srq_abo_case_one || m_srq_abo_case_two || m_srq_abo_case_three;
        }

        // Erase row from every chip's SRQ (used on full mitigation of a row).
        void erase_row_from_all_chip_srqs(int row) {
            for (auto& srq : m_chip_srqs) srq.erase(row);
        }

        // ---- MOAT counter-reset sweep ---------------------------------------
        //
        // Walk `m_prac_reset_rows_per_ref` rows forward each REFab and reset
        // their PRAC counters. Wraps at m_num_rows_per_bank. We only touch
        // counters that exist in the (sparse) m_counters map; entries that
        // were never materialized stay at the implicit 0. We also clear any
        // PSQ entry and stale critical-row marker for the rows being reset.
        void prac_reset_sweep_step() {
            const uint32_t rows_per_bank =
                  static_cast<uint32_t>(m_cfg.m_num_rows_per_bank);
            if (rows_per_bank == 0) return;   // Defensive.

            const uint32_t k = m_prac_reset_rows_per_ref;
            for (uint32_t i = 0; i < k; i++) {
                const int row = static_cast<int>(m_prac_reset_cursor);

                auto it = m_counters.find(row);
                if (it != m_counters.end()) {
                    it->second = 0;
                }
                // Drop any PSQ residue for this row; counter just hit 0.
                auto psq_it = m_psq.find(row);
                if (psq_it != m_psq.end()) m_psq.erase(psq_it);
                m_critical_rows.erase(row);

                m_prac_reset_cursor = (m_prac_reset_cursor + 1) % rows_per_bank;
                s_num_prac_counter_resets++;
            }
        }

        // ---- MINT (per-chip) -------------------------------------------------
        void mint_init() {
            m_mint_W = static_cast<uint32_t>(std::llround(1.0 / m_prob));
            if (m_mint_W == 0) m_mint_W = 1;

            m_mint_total_picks = 0;
            m_mint_pick_hist.assign(m_mint_W + 1, 0);

            m_chip_samplers.clear();
            m_chip_samplers.resize(m_num_chips);
            for (uint32_t chip = 0; chip < m_num_chips; chip++) {
                auto& s = m_chip_samplers[chip];
                s.step         = 0;
                s.pick         = 1;
                s.selected_row = -1;
                s.pick_seq     = 0;
                s.window_seq   = 0;
                s.gen.seed(0xC0FFEEu + static_cast<uint32_t>(m_bank_id) * 131u + chip);
                s.pick_dist    = std::uniform_int_distribution<uint32_t>(1, m_mint_W);
                draw_new_mint_pick(s, chip);
            }
        }

        void draw_new_mint_pick(MintSampler& sampler, uint32_t chip_id) {
            sampler.pick = sampler.pick_dist(sampler.gen);

            // Histogram is sized once in mint_init(); no per-call resize needed.
            m_mint_pick_hist[sampler.pick] += 1;
            m_mint_total_picks            += 1;
            sampler.pick_seq              += 1;

            if (m_debug) {
                std::printf("[DEBUG] MINT PICK bank=%d chip=%u pick_seq=%" PRIu64
                            " window_seq=%" PRIu64 " W=%u pick=%u\n",
                            m_bank_id, chip_id, sampler.pick_seq, sampler.window_seq,
                            m_mint_W, sampler.pick);
            }
        }

        // Each chip's sampler runs independently. At end of window, the chip
        // commits its selected row to ITS OWN SRQ (per-chip SRQ; Appendix-B).
        void mint_on_activation(int row_addr) {
            for (uint32_t chip = 0; chip < m_num_chips; chip++) {
                auto& sampler = m_chip_samplers[chip];

                sampler.step++;
                if (sampler.step == sampler.pick) {
                    sampler.selected_row = row_addr;
                }
                if (sampler.step < m_mint_W) continue;

                if (sampler.selected_row != -1) {
                    srq_commit_selected_row(chip, sampler.selected_row);
                }
                sampler.step         = 0;
                sampler.selected_row = -1;
                sampler.window_seq++;
                draw_new_mint_pick(sampler, chip);
            }
        }

        void srq_commit_selected_row(uint32_t chip_id, int row_addr) {
            auto& srq = m_chip_srqs[chip_id];
            auto it = srq.find(row_addr);
            if (it != srq.end()) {
                it->second.sc_counter++;
            } else if (srq.size() < m_srq_size) {
                // Initial insertion: SCtr=1 so the drain credits this selection's
                // worth of activations (1 + SCtr*W) when later drained.
                srq[row_addr] = SRQEntry{0, 1};
            }
            s_num_srq_update++;
        }

        // ---- SRQ drain (per-chip) -------------------------------------------
        //
        // Each chip drains up to `k` of its own SRQ entries, ordered by
        // ACtr desc (most-aged first), matching the MoPAC paper. Ties are
        // broken by SCtr desc, then by row id for deterministic ordering.
        int drain_srq(int k, int abo_type) {
            bool drained_any = false;

            struct DrainEntry { int row; uint32_t act; uint32_t sc; uint64_t total; };

            for (uint32_t chip = 0; chip < m_num_chips; chip++) {
                auto& srq = m_chip_srqs[chip];
                if (srq.empty()) continue;

                if (m_debug) {
                    const char* reason = (abo_type == 1) ? "TRR_REFab"
                                       : (abo_type == 2) ? "TTH"
                                       : "RFM_or_SRQfull";
                    std::printf("[SRQ_DRAIN] reason=%s bank=%d chip=%u k=%d srq_before=%zu\n",
                                reason, m_bank_id, chip, k, srq.size());
                }

                std::vector<DrainEntry> entries;
                entries.reserve(srq.size());
                for (const auto& [row, e] : srq) {
                    const uint64_t total =
                        static_cast<uint64_t>(e.act_counter) +
                        static_cast<uint64_t>(e.sc_counter) * static_cast<uint64_t>(m_mint_W);
                    entries.push_back({row, e.act_counter, e.sc_counter, total});
                }

                std::sort(entries.begin(), entries.end(),
                          [](const DrainEntry& a, const DrainEntry& b) {
                              if (a.act != b.act) return a.act > b.act;
                              if (a.sc  != b.sc)  return a.sc  > b.sc;
                              return a.row < b.row;  // deterministic tiebreak
                          });

                const int n = std::min<int>(k, static_cast<int>(entries.size()));
                for (int i = 0; i < n; i++) {
                    const int row = entries[i].row;

                    // PRAC counter += 1 + SCtr * (1/p) = 1 + SCtr * W.
                    const uint32_t inc = 1u + entries[i].sc * m_mint_W;
                    m_counters[row] += inc;

                    if (m_debug) {
                        std::printf("[Debug] DRAIN bank=%d chip=%u row=%d ctr=%u "
                                    "(ACtr=%u SCtr=%u inc=%u)\n",
                                    m_bank_id, chip, row, m_counters[row],
                                    entries[i].act, entries[i].sc, inc);
                    }

                    (void) update_psq(row);
                    srq.erase(row);
                    drained_any = true;
                }
            }

            return drained_any ? 1 : -1;
        }

        // ---- PSQ (MOAT) management -------------------------------------------
        bool is_psq_full() const { return m_psq.size() >= m_psq_size; }

        bool replace_psq_entry(int row_addr) {
            if (m_psq.empty()) return false;

            auto min_entry = std::min_element(
                m_psq.begin(), m_psq.end(),
                [](const auto& a, const auto& b) { return a.second < b.second; });

            if (m_counters[row_addr] > min_entry->second) {
                if (min_entry->first != row_addr) {
                    m_psq[row_addr] = m_counters[row_addr];
                    if (min_entry->second >= ath_shared_scale()) {
                        m_critical_rows.erase(min_entry->first);
                    }
                    m_psq.erase(min_entry->first);
                } else {
                    m_psq[row_addr] = m_counters[row_addr];
                }
                return true;
            }
            return false;
        }

        // Returns: 0 = updated existing entry, 1 = inserted into empty slot,
        //          2 = inserted via replacement, -1 = not inserted.
        //
        // MOAT insertion rule: a row is only NEWLY inserted into the PSQ once
        // its PRAC counter has reached `m_insertion_th`. Rows already in the
        // PSQ are always kept in sync (no threshold check).
        int update_psq(int row_addr) {
            auto it = m_psq.find(row_addr);
            if (it != m_psq.end()) {
                it->second = m_counters[row_addr];
                return 0;
            }

            // MOAT: gate new insertions on the insertion threshold.
            if (m_counters[row_addr] < insertion_th_shared_scale()) return -1;

            if (m_psq.size() < m_psq_size) {
                m_psq[row_addr] = m_counters[row_addr];
                return 1;
            }
            return replace_psq_entry(row_addr) ? 2 : -1;
        }

        void bump_victim_counter(int victim_row) {
            if (victim_row < 0 || victim_row >= m_cfg.m_num_rows_per_bank) return;

            auto& ctr = m_counters[victim_row];   // Default-inserts to 0 if absent.
            // Shared-sum model: a victim refresh bumps EACH chip's per-row
            // PRAC counter by 1, so the shared sum rises by num_chips.
            ctr += m_num_chips;

            const int update_type = update_psq(victim_row);
            if (update_type < 0) return;          // Not tracked in PSQ; nothing else to do.

            if (ctr >= ath_shared_scale()) {
                if (ctr != m_psq[victim_row]) {
                    std::printf("[VICTIM][ERROR] PSQ counter %u != in-DRAM counter %u (row=%d)\n",
                                m_psq[victim_row], ctr, victim_row);
                    assert(ctr == m_psq[victim_row]);
                }
                m_critical_rows[victim_row] = ctr;
                s_num_case_two++;
                m_is_abo_needed = true;
            }
        }

        // Bump the +/- 1 and +/- 2 victim rows of an aggressor (blast radius 2).
        void increase_victim_counters(int row_addr) {
            for (int d = 1; d <= 2; d++) {
                bump_victim_counter(row_addr - d);
                bump_victim_counter(row_addr + d);
            }
        }

        // ---- Mitigation paths (RFM-driven and TRR-driven) --------------------
        // mitigation_type: 0 = RFM (ABO recovery), 1 = TRR (REFab drain-on-REF).
        void process_psq_mitigation(int mitigation_type) {
            if (mitigation_type == 1) {
                if (m_debug) std::printf("[Debug] TRR drains SRQ\n");
                drain_srq(m_num_drain, 1);
                refresh_local_abo_reasons();
                return;
            }

            refresh_local_abo_reasons();
            const bool local_trigger = has_local_abo_reason();

            if (local_trigger) {
                if (m_srq_abo_case_one || m_srq_abo_case_three) {
                    if (get_total_srq_size() > 0) {
                        drain_srq(kAboDrainMax, m_srq_abo_case_one ? 0 : 2);
                    }
                } else if (m_srq_abo_case_two) {
                    auto moat = find_moat();
                    if (moat != m_psq.end() &&
                        moat->second >= ath_shared_scale()) {
                        const int row_addr = moat->first;

                        if (m_debug) {
                            std::printf("[Debug] CASE2 MOAT mitigation row=%d ctr=%u\n",
                                        row_addr, m_counters[row_addr]);
                        }

                        m_counters[row_addr] = 0;
                        m_critical_rows.erase(row_addr);
                        m_psq.erase(row_addr);
                        erase_row_from_all_chip_srqs(row_addr);

                        s_num_total_mitigations++;
                        increase_victim_counters(row_addr);
                    }
                }
            } else {
                if (!m_enable_opportunistic_mitigation) {
                    refresh_local_abo_reasons();
                    return;
                }

                // Opportunistic: prefer to drain SRQ; if all chips empty, mitigate MOAT row.
                if (get_total_srq_size() > 0) {
                    drain_srq(kAboDrainMax, 0);
                } else if (!m_psq.empty()) {
                    auto moat = find_moat();
                    const int row_addr = moat->first;

                    m_counters[row_addr] = 0;
                    m_critical_rows.erase(row_addr);
                    m_psq.erase(row_addr);
                    erase_row_from_all_chip_srqs(row_addr);

                    increase_victim_counters(row_addr);
                    s_num_total_mitigations++;
                }
            }

            refresh_local_abo_reasons();
            m_abo_this_bank = has_local_abo_reason();
        }

        void process_targeted_ref(const Request& /*req*/) {
            // MOAT's optional counter reset at refresh. Independent of the
            // targeted-refresh path: runs on EVERY REFab when enabled.
            if (m_enable_prac_reset_at_refresh) {
                prac_reset_sweep_step();
            }

            if (m_targeted_ref_frequency == 0) return;

            m_num_ref++;
            if (m_num_ref % m_targeted_ref_frequency != 0) return;

            if (m_debug) {
                std::printf("[TRR] bank=%d ref_cnt=%" PRIu64 " srq_total=%" PRIu64 " psq=%zu\n",
                            m_bank_id, m_num_ref, get_total_srq_size(), m_psq.size());
            }
            process_psq_mitigation(1);
            if (m_bank_id == 0) {
                s_num_targeted_ref += m_cfg.m_num_ranks;
            }
        }

        // ---- ACT handler -----------------------------------------------------
        void process_act(const Request& req) {
            s_num_acts++;
            const int row_addr = req.addr_vec[m_cfg.m_row_level];

            // Lazily materialize the counter for this row. m_counters[row] would
            // also do this via operator[], but we prefer an explicit form so we
            // don't accidentally insert in read-only paths elsewhere.
            if (m_counters.find(row_addr) == m_counters.end()) {
                m_counters[row_addr] = 0;
            }

            // ACtr++: every chip whose SRQ already holds this row increments its
            // own per-chip ACtr (shared bus -> shared activation event).
            for (auto& srq : m_chip_srqs) {
                auto it = srq.find(row_addr);
                if (it != srq.end()) {
                    it->second.act_counter++;
                    if (m_debug) {
                        std::printf("[DEBUG] SRQ_HIT row=%d ACtr=%u SCtr=%u\n",
                                    row_addr, it->second.act_counter, it->second.sc_counter);
                    }
                }
            }

            // MINT: each chip's sampler steps. May commit at end of window.
            mint_on_activation(row_addr);

            // Case 1: any chip's SRQ full?
            if (is_any_chip_srq_full()) {
                if (!m_srq_abo_case_one) {
                    m_srq_abo_case_one = true;
                    s_num_case_one++;
                    if (m_debug) {
                        std::printf("[DEBUG] CASE1 SRQ full (some chip), total_srq=%" PRIu64 "\n",
                                    get_total_srq_size());
                    }
                }
            }

            // Case 2: MOAT row at >= ATH*.
            if (!m_psq.empty()) {
                auto moat = find_moat();
                if (moat != m_psq.end() &&
                    moat->second >= ath_shared_scale()) {
                    if (m_debug) {
                        std::printf("[Debug] CASE2 PSQ row=%d ctr=%u\n",
                                    moat->first, m_counters[moat->first]);
                    }
                    if (!m_srq_abo_case_two) {
                        m_srq_abo_case_two = true;
                        s_num_case_two++;
                    }
                    m_critical_rows[moat->first] = m_counters[moat->first];
                    m_abo_row_addr = moat->first;
                }
            }

            // Case 3: ACtr >= TTH on this row in some chip's SRQ.
            // Bug fix vs. previous version: guard against TTH == 0 (which would
            // otherwise make every in-SRQ activation satisfy ACtr >= 0).
            if (m_tardiness_th > 0) {
                for (const auto& srq : m_chip_srqs) {
                    auto it = srq.find(row_addr);
                    if (it != srq.end() &&
                        it->second.act_counter >= m_tardiness_th) {
                        if (!m_srq_abo_case_three) {
                            m_srq_abo_case_three = true;
                            s_num_case_three++;
                            if (m_debug) {
                                std::printf("[DEBUG] CASE3 TTH hit row=%d ACtr=%u TTH=%u\n",
                                            row_addr, it->second.act_counter,
                                            m_tardiness_th);
                            }
                        }
                        break;
                    }
                }
            }

            // Aggregate ABO need.
            if (m_srq_abo_case_one || m_srq_abo_case_two || m_srq_abo_case_three) {
                m_is_abo_needed = true;
                m_abo_this_bank = true;
                if (m_debug) {
                    std::printf("[Debug] ABO REASONS: c1=%s c2=%s c3=%s\n",
                                m_srq_abo_case_one   ? "true" : "false",
                                m_srq_abo_case_two   ? "true" : "false",
                                m_srq_abo_case_three ? "true" : "false");
                }
            } else {
                if (is_critical()) s_num_remained_critical_rows++;
                m_abo_this_bank = false;
            }
        }

        void process_rfm(const Request& /*req*/) {
            process_psq_mitigation(0);
        }

    private:
        // ---- State -----------------------------------------------------------
        DeviceConfig& m_cfg;
        bool&         m_is_abo_needed;

        std::unordered_map<int, uint32_t> m_counters;       // PRAC counters (shared across chips).
        std::unordered_map<int, uint32_t> m_critical_rows;  // Rows with counter >= ATH*.
        std::unordered_map<int, uint32_t> m_psq;            // MOAT / Priority Service Queue.

        // Per-chip SRQs (Appendix-B). Each chip independently fills its own SRQ.
        std::vector<std::unordered_map<int, SRQEntry>> m_chip_srqs;

        bool m_srq_abo_case_one   = false;  // Some chip's SRQ is full.
        bool m_srq_abo_case_two   = false;  // MOAT row >= ATH*.
        bool m_srq_abo_case_three = false;  // Some chip's SRQ has ACtr >= TTH.
        bool m_abo_this_bank      = false;
        int  m_abo_row_addr       = -1;

        // MINT state.
        uint32_t m_mint_W = 1;
        std::vector<MintSampler> m_chip_samplers;
        uint64_t m_mint_total_picks = 0;
        std::vector<uint64_t> m_mint_pick_hist;

        std::uniform_real_distribution<double> prob_dist;   // (Reserved for future use.)

        // ---- Constants & configuration --------------------------------------
        double   m_prob              = 0.0;
        uint32_t m_srq_size          = 0;
        int      m_num_drain         = 0;
        uint32_t m_num_chips         = 1;

        int   m_alert_thresh = -1;
        bool  m_debug        = false;
        int   m_bank_id      = -1;

        uint32_t m_psq_size                  = 0;
        uint32_t m_insertion_th              = 0;
        uint32_t m_tardiness_th   = 32;
        bool     m_enable_opportunistic_mitigation = true;

        // MOAT: refresh-time PRAC counter reset.
        bool     m_enable_prac_reset_at_refresh = false;
        uint32_t m_prac_reset_rows_per_ref      = 0;   // = num_rows / 8192 (capped >=1).
        uint32_t m_prac_reset_cursor            = 0;   // Next row to reset; wraps.

        uint32_t m_targeted_ref_frequency = 0;
        uint64_t m_num_ref                = 0;

        // Cached command IDs (set at construction / init_dram_params).
        int m_cmd_act    = -1;
        int m_cmd_rfmab  = -1;
        int m_cmd_rfmsb  = -1;
        int m_cmd_refab  = -1;

        // References to controller-level stats.
        uint64_t& s_num_targeted_ref;
        uint64_t& s_num_total_mitigations;
        uint64_t& s_num_prac_counter_resets;

    public:
        // Test/debug accessors (kept from the original interface).
        const std::vector<uint64_t>& get_mint_pick_hist()  const { return m_mint_pick_hist; }
        uint64_t                     get_mint_total_picks() const { return m_mint_total_picks; }
        uint32_t                     get_mint_window_size() const { return m_mint_W; }
    };  // class PerBankCounters
};      // class MOPAC

uint64_t MOPAC::s_num_acts                  = 0;
uint64_t MOPAC::s_num_srq_update            = 0;
uint64_t MOPAC::s_num_remained_critical_rows = 0;
uint64_t MOPAC::s_num_case_one              = 0;
uint64_t MOPAC::s_num_case_two              = 0;
uint64_t MOPAC::s_num_case_three            = 0;
double   MOPAC::s_avg_srq_update            = 0.0;

}  // namespace Ramulator
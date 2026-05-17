#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/plugin.h"
#include "dram_controller/impl/plugin/device_config/device_config.h"
#include "dram_controller/impl/plugin/prism/prism.h"

#include <limits>
#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <random>

namespace Ramulator {

class PRISM : public IControllerPlugin, public Implementation, public IPRISM {
    RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, PRISM, "PRISM", "PrISM ")

private:
    class PerBankStructures;

private:
    Clk_t m_clk = 0;

    DeviceConfig m_cfg;
    std::vector<PRISM::PerBankStructures> m_bank_structures;

    // ------------------- ABO state machine -----------------------------------
    ABOState m_state = ABOState::NORMAL;

    // Clk at which RECOVERY begins (NORMAL/PRE_RECOVERY transition target).
    // Reported via next_recovery_cycle() so the controller can schedule PREA
    // before this deadline and start issuing RFMs at this cycle.
    Clk_t m_abo_recovery_start = std::numeric_limits<Clk_t>::max();

    // ABO timing/protocol params
    int m_abo_act_ns        = -1; // tABO_ACT, ns. JEDEC says <=180ns.
    int m_abo_recovery_refs = -1; // RFMs issued during RECOVERY per Alert.
                                  // PrISM: 1.
    int m_abo_delay_acts    = -1; // ABO_Delay ACTs after RECOVERY before
                                  // Alert may be reasserted. PrISM: 1.

    int m_abo_act_cycles    = -1; // tABO_ACT converted to cycles

    // Remaining RFMs to drain during RECOVERY (sum across ranks).
    uint32_t m_abo_recov_rem_refs = 0;
    // Remaining ACTs to count down during DELAY.
    uint32_t m_abo_delay_rem_acts = 0;

    // Global ABO flag: OR of per-bank local ABO needs.
    bool m_is_abo_needed = false;

    bool m_debug = false;

    // ------------------- PrISM parameters ------------------------------------
    int m_max_acts_per_mitigation     = -1; // W: ACTs per window
    int m_sampled_rows_per_mitigation = -1; // R: sampled slots per window
    int m_shq_length                  = -1; // L: SHQ lookback length
    int m_shq_capacity                = -1; // L * (R - 1)

    int m_pmq_capacity                = -1; // PMQ capacity (Q)
    int m_pmq_threshold               = -1; // T_PMQ

    int m_ssq_capacity_override       = -1; // optional user override, -1 = auto
    int m_ssq_capacity                = -1; // resolved at setup()

    int m_targeted_ref_frequency      = -1; // 1 TRR per N REFs

    // ------------------- Stats -----------------------------------------------
    // ABO / mitigation totals
    uint64_t s_num_recovery        = 0;
    uint64_t s_num_requested_RFMs  = 0;
    uint64_t s_num_useful_abo_rfms = 0;
    uint64_t s_num_wasted_abo_rfms = 0;

    uint64_t s_num_targeted_ref      = 0;
    uint64_t s_num_total_mitigations = 0;

    // Mitigation source breakdown
    uint64_t s_num_pmq_mitigations_abo           = 0;
    uint64_t s_num_pmq_mitigations_opportunistic = 0;
    uint64_t s_num_sampled_candidate_mitigations = 0; // fallback: SSQ candidate
    uint64_t s_num_wasted_abo_slots              = 0;

    // PMQ-side events
    uint64_t s_num_pmq_inserts_intersection_immediate = 0;
    uint64_t s_num_pmq_inserts_intersection_deferred  = 0;
    uint64_t s_num_pmq_inserts_default                = 0;
    uint64_t s_num_pmq_merges                         = 0;
    uint64_t s_num_pmq_threshold_crossings            = 0;

    uint64_t s_num_alert_events_pmq_qth   = 0;
    uint64_t s_num_alert_events_pmq_full  = 0;
    uint64_t s_num_alert_events_multi_src = 0;

    // SSQ-side events
    uint64_t s_ssq_pending_intersection_max = 0;
    uint64_t s_ssq_max_occupancy            = 0;
    uint64_t s_ssq_drain_to_pmq             = 0;

    // PMQ instantaneous occupancy (sampled on each PMQ touch)
    uint64_t s_pmq_occupancy_samples = 0;
    uint64_t s_pmq_occupancy_sum     = 0;
    uint64_t s_pmq_occupancy_max     = 0;

    // PMQ time-weighted occupancy (sampled every cycle, all banks)
    uint64_t s_pmq_tw_cycles              = 0;
    uint64_t s_pmq_tw_bank_cycles         = 0;
    uint64_t s_pmq_tw_occupancy_sum       = 0;
    uint64_t s_pmq_tw_total_occupancy_max = 0;
    uint64_t s_pmq_tw_per_bank_max        = 0;

    uint64_t s_pmq_tw_occ_ge_1        = 0;
    uint64_t s_pmq_tw_occ_ge_2        = 0;
    uint64_t s_pmq_tw_occ_ge_4        = 0;
    uint64_t s_pmq_tw_occ_ge_8        = 0;
    uint64_t s_pmq_tw_occ_ge_16       = 0;
    uint64_t s_pmq_tw_occ_ge_32       = 0;
    uint64_t s_pmq_tw_occ_at_capacity = 0;

public:
    void init() override {
        m_debug = param<bool>("debug").default_val(false);

        // PrISM core parameters (camera-ready defaults)
        m_max_acts_per_mitigation     = param<int>("max_acts_per_mitigation").default_val(72);
        m_sampled_rows_per_mitigation = param<int>("sampled_rows_per_mitigation").default_val(5);
        m_shq_length                  = param<int>("shq_length").default_val(8);
        m_pmq_capacity                = param<int>("pmq_capacity").default_val(16);
        m_pmq_threshold               = param<int>("pmq_threshold").default_val(4);

        // SSQ: -1 means auto-derive from R. Otherwise the user override must
        // still satisfy the required bound; we assert this in setup().
        m_ssq_capacity_override       = param<int>("ssq_capacity").default_val(-1);

        // ABO protocol params (camera-ready: 1 RFM per Alert, 1 ABO_Delay ACT,
        // tABO_ACT <= 180 ns per JEDEC).
        m_abo_act_ns         = param<int>("abo_act_ns").default_val(180);
        m_abo_recovery_refs  = param<int>("abo_recovery_refs").default_val(1);
        m_abo_delay_acts     = param<int>("abo_delay_acts").default_val(1);

        // Targeted refresh frequency
        m_targeted_ref_frequency = param<uint32_t>("targeted_ref_frequency").default_val(1);
    }

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
        m_cfg.set_device(cast_parent<IDRAMController>());

        m_is_abo_needed = false;
        m_abo_act_cycles = m_abo_act_ns /
            ((float) m_cfg.m_dram->m_timing_vals("tCK_ps") / 1000.0f);

        // -------- Resolve SSQ capacity from R --------
        // Per camera-ready Eq. (1):  S_SSQ >= (2R - 1) - floor((2R - 1) / 4)
        const int R = m_sampled_rows_per_mitigation;
        const int required_ssq = (2 * R - 1) - ((2 * R - 1) / 4);

        if (m_ssq_capacity_override < 0) {
            m_ssq_capacity = required_ssq;
        } else {
            // User override is allowed only if it is >= the required bound.
            if (m_ssq_capacity_override < required_ssq) {
                std::cerr << "[PrISM][CRITICAL] ssq_capacity override "
                          << m_ssq_capacity_override
                          << " is below the required bound " << required_ssq
                          << " for R=" << R << ".\n";
                std::exit(EXIT_FAILURE);
            }
            m_ssq_capacity = m_ssq_capacity_override;
        }

        // -------- Sanity check W >= 4R (long-run intersection rate bound) ----
        if (m_max_acts_per_mitigation < 4 * R) {
            std::cerr << "[PrISM][WARN] W=" << m_max_acts_per_mitigation
                      << " is less than 4R=" << (4 * R)
                      << "; long-run intersection rate may exceed ABO drain rate.\n";
        }

        m_shq_capacity = m_shq_length * (R - 1);

        // -------- Register stats --------
        register_stat(s_num_targeted_ref).name("prism_num_targeted_ref");
        register_stat(s_num_total_mitigations).name("prism_num_total_mitigations");

        register_stat(s_num_recovery).name("prism_num_abo_recovery");
        register_stat(s_num_requested_RFMs).name("prism_requested_abo_rfms");
        register_stat(s_num_useful_abo_rfms).name("prism_useful_abo_rfms");
        register_stat(s_num_wasted_abo_rfms).name("prism_wasted_abo_rfms");

        register_stat(s_num_pmq_mitigations_abo).name("prism_pmq_mitigations_abo");
        register_stat(s_num_pmq_mitigations_opportunistic).name("prism_pmq_mitigations_opportunistic");
        register_stat(s_num_sampled_candidate_mitigations).name("prism_sampled_candidate_mitigations_opportunistic");
        register_stat(s_num_wasted_abo_slots).name("prism_wasted_abo_slots");

        register_stat(s_num_pmq_inserts_intersection_immediate).name("prism_pmq_inserts_intersection_immediate");
        register_stat(s_num_pmq_inserts_intersection_deferred).name("prism_pmq_inserts_intersection_deferred");
        register_stat(s_num_pmq_inserts_default).name("prism_pmq_inserts_default");
        register_stat(s_num_pmq_merges).name("prism_pmq_merges");
        register_stat(s_num_pmq_threshold_crossings).name("prism_pmq_threshold_crossings");

        register_stat(s_num_alert_events_pmq_qth).name("prism_alert_events_pmq_qth");
        register_stat(s_num_alert_events_pmq_full).name("prism_alert_events_pmq_full");
        register_stat(s_num_alert_events_multi_src).name("prism_alert_events_multi_source");

        register_stat(s_ssq_pending_intersection_max).name("prism_ssq_pending_intersection_max");
        register_stat(s_ssq_max_occupancy).name("prism_ssq_max_occupancy");
        register_stat(s_ssq_drain_to_pmq).name("prism_ssq_drain_to_pmq");

        register_stat(s_pmq_occupancy_samples).name("prism_pmq_occupancy_samples");
        register_stat(s_pmq_occupancy_sum).name("prism_pmq_occupancy_sum");
        register_stat(s_pmq_occupancy_max).name("prism_pmq_occupancy_max");

        register_stat(s_pmq_tw_cycles).name("prism_pmq_tw_cycles");
        register_stat(s_pmq_tw_bank_cycles).name("prism_pmq_tw_bank_cycles");
        register_stat(s_pmq_tw_occupancy_sum).name("prism_pmq_tw_occupancy_sum");
        register_stat(s_pmq_tw_total_occupancy_max).name("prism_pmq_tw_total_occupancy_max");
        register_stat(s_pmq_tw_per_bank_max).name("prism_pmq_tw_per_bank_max");

        register_stat(s_pmq_tw_occ_ge_1).name("prism_pmq_tw_occ_ge_1");
        register_stat(s_pmq_tw_occ_ge_2).name("prism_pmq_tw_occ_ge_2");
        register_stat(s_pmq_tw_occ_ge_4).name("prism_pmq_tw_occ_ge_4");
        register_stat(s_pmq_tw_occ_ge_8).name("prism_pmq_tw_occ_ge_8");
        register_stat(s_pmq_tw_occ_ge_16).name("prism_pmq_tw_occ_ge_16");
        register_stat(s_pmq_tw_occ_ge_32).name("prism_pmq_tw_occ_ge_32");
        register_stat(s_pmq_tw_occ_at_capacity).name("prism_pmq_tw_occ_at_capacity");

        // -------- Construct per-bank structures --------
        m_bank_structures.reserve(m_cfg.m_num_banks);
        for (int i = 0; i < m_cfg.m_num_banks; i++) {
            m_bank_structures.emplace_back(
                i, m_cfg, m_debug,
                m_targeted_ref_frequency,
                m_max_acts_per_mitigation,
                m_sampled_rows_per_mitigation,
                m_shq_length,
                m_pmq_capacity,
                m_pmq_threshold,
                m_ssq_capacity,
                s_num_targeted_ref,
                s_num_total_mitigations,
                s_num_pmq_mitigations_abo,
                s_num_pmq_mitigations_opportunistic,
                s_num_sampled_candidate_mitigations,
                s_num_wasted_abo_slots,
                s_num_pmq_inserts_intersection_immediate,
                s_num_pmq_inserts_intersection_deferred,
                s_num_pmq_inserts_default,
                s_num_pmq_merges,
                s_num_pmq_threshold_crossings,
                s_ssq_pending_intersection_max,
                s_ssq_max_occupancy,
                s_ssq_drain_to_pmq,
                s_pmq_occupancy_samples,
                s_pmq_occupancy_sum,
                s_pmq_occupancy_max
            );
        }

        std::cout << "[PrISM] setup: W=" << m_max_acts_per_mitigation
                << " R=" << R
                << " L=" << m_shq_length
                << " SHQ_capacity=" << m_shq_capacity
                << " PMQ_capacity=" << m_pmq_capacity
                << " T_PMQ=" << m_pmq_threshold
                << " SSQ_capacity=" << m_ssq_capacity
                << " (required>=" << required_ssq << ")"
                << " tABO_ACT(cyc)=" << m_abo_act_cycles
                << " ABO_recovery_RFMs=" << m_abo_recovery_refs
                << " ABO_Delay_ACTs=" << m_abo_delay_acts
                << "\n";
    }

    // Helper: compute the OR of per-bank local ABO requests.
    void check_global_abo_status() {
        m_is_abo_needed = false;
        for (const auto& bank : m_bank_structures) {
            if (bank.is_local_abo_needed()) {
                m_is_abo_needed = true;
                break;
            }
        }
    }

    void record_time_weighted_pmq_occupancy() {
        uint64_t total_occ_this_cycle = 0;
        for (const auto& bank : m_bank_structures) {
            const int occ = bank.get_pmq_occupancy();
            total_occ_this_cycle += static_cast<uint64_t>(occ);

            s_pmq_tw_per_bank_max =
                std::max(s_pmq_tw_per_bank_max, static_cast<uint64_t>(occ));

            if (occ >= 1)  s_pmq_tw_occ_ge_1++;
            if (occ >= 2)  s_pmq_tw_occ_ge_2++;
            if (occ >= 4)  s_pmq_tw_occ_ge_4++;
            if (occ >= 8)  s_pmq_tw_occ_ge_8++;
            if (occ >= 16) s_pmq_tw_occ_ge_16++;
            if (occ >= 32) s_pmq_tw_occ_ge_32++;
            if (occ >= m_pmq_capacity) s_pmq_tw_occ_at_capacity++;
        }
        s_pmq_tw_cycles++;
        s_pmq_tw_bank_cycles +=
            static_cast<uint64_t>(m_bank_structures.size());
        s_pmq_tw_occupancy_sum += total_occ_this_cycle;
        s_pmq_tw_total_occupancy_max =
            std::max(s_pmq_tw_total_occupancy_max, total_occ_this_cycle);
    }

    void update(bool request_found, ReqBuffer::iterator& req_it) override {
        m_clk++;

        check_global_abo_status();

        const Request* req_ptr = request_found ? &(*req_it) : nullptr;
        update_state_machine(request_found, req_ptr);

        if (request_found) {
            auto& req = *req_it;

            bool has_bank_wildcard      = req.addr_vec[m_cfg.m_bank_level] == -1;
            bool has_bankgroup_wildcard = req.addr_vec[m_cfg.m_bankgroup_level] == -1;

            if (has_bankgroup_wildcard && has_bank_wildcard) {
                // All BG, All Bank in this rank (All Bank)
                int offset = req.addr_vec[m_cfg.m_rank_level] * m_cfg.m_num_banks_per_rank;
                for (int i = 0; i < m_cfg.m_num_banks_per_rank; i++) {
                    m_bank_structures[offset + i].on_request(req);
                }
                req.addr_vec[m_cfg.m_bank_level] = -1;
            } else if (has_bankgroup_wildcard) {
                // All BG, Single Bank (Same Bank Command: REFsb / RFMsb)
                int rank_offset = req.addr_vec[m_cfg.m_rank_level] * m_cfg.m_num_banks_per_rank;
                int bank_offset = req.addr_vec[m_cfg.m_bank_level];
                for (int i = 0; i < m_cfg.m_num_bankgroups; i++) {
                    int bg_offset = i * m_cfg.m_num_banks_per_bankgroup;
                    m_bank_structures[rank_offset + bg_offset + bank_offset].on_request(req);
                }
            } else if (has_bank_wildcard) {
                // Single BG, All Bank
                int rank_offset = req.addr_vec[m_cfg.m_rank_level] * m_cfg.m_num_banks_per_rank;
                int bg_offset   = req.addr_vec[m_cfg.m_bankgroup_level] * m_cfg.m_num_banks_per_bankgroup;
                for (int i = 0; i < m_cfg.m_num_banks_per_bankgroup; i++) {
                    m_bank_structures[rank_offset + bg_offset + i].on_request(req);
                }
            } else {
                // Single BG, Single Bank (Per-Bank Command: REFpb)
                auto flat_bank_id = m_cfg.get_flat_bank_id(req);
                m_bank_structures[flat_bank_id].on_request(req);
            }
        }

        record_time_weighted_pmq_occupancy();
    }

    // ------------------- 4-state ABO state machine -------------------------
    // NORMAL --(local ABO asserted)--> PRE_RECOVERY
    // PRE_RECOVERY --(tABO_ACT elapsed)--> RECOVERY
    // RECOVERY --(all RFMs drained)--> DELAY
    // DELAY --(ABO_Delay ACTs observed)--> NORMAL
    // -----------------------------------------------------------------------
    void update_state_machine(bool request_found, const Request* req) {
        static const std::unordered_map<ABOState, std::string> state_names = {
            {ABOState::NORMAL,       "ABOState::NORMAL"},
            {ABOState::PRE_RECOVERY, "ABOState::PRE_RECOVERY"},
            {ABOState::RECOVERY,     "ABOState::RECOVERY"},
            {ABOState::DELAY,        "ABOState::DELAY"}
        };

        auto cmd_act   = m_cfg.m_dram->m_commands("ACT");
        auto cmd_prea  = m_cfg.m_dram->m_commands("PREA");
        auto cmd_rfmab = m_cfg.m_dram->m_commands("RFMab");
        // auto cmd_rfmsb = m_cfg.m_dram->m_commands("RFMsb"); // RFMsb is not supported in current ABO protocol

        ABOState cur_state = m_state;

        switch (m_state) {
        case ABOState::NORMAL:
            if (m_is_abo_needed) {
                if (m_debug) {
                    std::printf("[PrISM] [%lu] <%s> Asserting ALERT_N.\n",
                                m_clk, state_names.at(cur_state).c_str());
                }
                // Telemetry: snapshot which source(s) triggered this Alert.
                snapshot_alert_sources();

                m_state = ABOState::PRE_RECOVERY;
                m_abo_recovery_start = m_clk + m_abo_act_cycles;
                s_num_recovery++;
            }
            break;

        case ABOState::PRE_RECOVERY:
            // Controller is expected to drain in-flight ACTs and issue PREA
            // within tABO_ACT. 
            if (request_found && req != nullptr && req->command == cmd_prea) {
                if (m_debug) {
                    std::printf("[PrISM] [%lu] <%s> Observed PREA.\n",
                                m_clk, state_names.at(cur_state).c_str());
                }
            }
            if (m_clk == m_abo_recovery_start) {
                m_state = ABOState::RECOVERY;
                m_abo_recovery_start = std::numeric_limits<Clk_t>::max();
                m_abo_recov_rem_refs =
                    m_abo_recovery_refs * m_cfg.m_num_ranks;
                s_num_requested_RFMs += m_abo_recov_rem_refs;
            }
            break;

        case ABOState::RECOVERY:
            if (request_found && req != nullptr && (req->command == cmd_rfmab)) {
                if (m_abo_recov_rem_refs > 0) {
                    m_abo_recov_rem_refs--;
                }
                if (m_abo_recov_rem_refs == 0) {
                    m_state = ABOState::DELAY;
                    m_abo_delay_rem_acts = m_abo_delay_acts;
                }
            }
            break;

        case ABOState::DELAY:
            if (request_found && req != nullptr && req->command == cmd_act) {
                if (m_abo_delay_rem_acts > 0) {
                    m_abo_delay_rem_acts--;
                }
                if (m_abo_delay_rem_acts == 0) {
                    // Recompute global ABO flag based on the current state of
                    // the per-bank structures.
                    check_global_abo_status();
                    m_state = ABOState::NORMAL;
                }
            }
            break;
        }

        if (m_debug && cur_state != m_state) {
            std::printf("[PrISM] [%lu] <%s> -> <%s>\n",
                        m_clk,
                        state_names.at(cur_state).c_str(),
                        state_names.at(m_state).c_str());
        }
    }

    // Telemetry: capture which alert source(s) triggered this Alert.
    void snapshot_alert_sources() {
        bool event_pmq_qth  = false;
        bool event_pmq_full = false;
        for (const auto& b : m_bank_structures) {
            event_pmq_qth  = event_pmq_qth  || b.has_pmq_threshold_alert();
            event_pmq_full = event_pmq_full || b.has_pmq_capacity_alert();
        }
        if (event_pmq_qth)  s_num_alert_events_pmq_qth++;
        if (event_pmq_full) s_num_alert_events_pmq_full++;
        if (event_pmq_qth && event_pmq_full) s_num_alert_events_multi_src++;
    }

    // -------- IPRISM interface ----------------------------------------------
    int get_num_abo_recovery_refs() override {
        // Per-rank value; controller multiplies by ranks as needed.
        return m_abo_recovery_refs;
    }

    Clk_t next_recovery_cycle() override {
        return m_abo_recovery_start;
    }

    ABOState get_state() override { return m_state; }

    void finalize() override {}

private:
    // =========================================================================
    //  PER-BANK STRUCTURES
    // =========================================================================
    class PerBankStructures {
    public:
        PerBankStructures(
            int bank_id,
            DeviceConfig& cfg,
            bool debug,
            int targeted_ref_frequency,
            int max_acts_per_mitigation,
            int sampled_rows_per_mitigation,
            int shq_length,
            int pmq_capacity,
            int pmq_threshold,
            int ssq_capacity,
            uint64_t& num_targeted_ref,
            uint64_t& num_total_mitigations,
            uint64_t& num_pmq_mitigations_abo,
            uint64_t& num_pmq_mitigations_opportunistic,
            uint64_t& num_sampled_candidate_mitigations,
            uint64_t& num_wasted_abo_slots,
            uint64_t& num_pmq_inserts_intersection_immediate,
            uint64_t& num_pmq_inserts_intersection_deferred,
            uint64_t& num_pmq_inserts_default,
            uint64_t& num_pmq_merges,
            uint64_t& num_pmq_threshold_crossings,
            uint64_t& ssq_pending_intersection_max,
            uint64_t& ssq_max_occupancy,
            uint64_t& ssq_drain_to_pmq,
            uint64_t& pmq_occupancy_samples,
            uint64_t& pmq_occupancy_sum,
            uint64_t& pmq_occupancy_max)
        :   m_cfg(cfg),
            m_max_acts_per_mitigation(max_acts_per_mitigation),
            m_sampled_rows_per_mitigation(sampled_rows_per_mitigation),
            m_shq_length(shq_length),
            m_shq_capacity(shq_length * (sampled_rows_per_mitigation - 1)),
            m_pmq_threshold(pmq_threshold),
            m_pmq_capacity(pmq_capacity),
            m_ssq_capacity(ssq_capacity),
            m_targeted_ref_frequency(targeted_ref_frequency),
            gen(42 + bank_id),
            m_debug(debug),
            m_bank_id(bank_id),
            s_num_targeted_ref(num_targeted_ref),
            s_num_total_mitigations(num_total_mitigations),
            s_num_pmq_mitigations_abo(num_pmq_mitigations_abo),
            s_num_pmq_mitigations_opportunistic(num_pmq_mitigations_opportunistic),
            s_num_sampled_candidate_mitigations(num_sampled_candidate_mitigations),
            s_num_wasted_abo_slots(num_wasted_abo_slots),
            s_num_pmq_inserts_intersection_immediate(num_pmq_inserts_intersection_immediate),
            s_num_pmq_inserts_intersection_deferred(num_pmq_inserts_intersection_deferred),
            s_num_pmq_inserts_default(num_pmq_inserts_default),
            s_num_pmq_merges(num_pmq_merges),
            s_num_pmq_threshold_crossings(num_pmq_threshold_crossings),
            s_ssq_pending_intersection_max(ssq_pending_intersection_max),
            s_ssq_max_occupancy(ssq_max_occupancy),
            s_ssq_drain_to_pmq(ssq_drain_to_pmq),
            s_pmq_occupancy_samples(pmq_occupancy_samples),
            s_pmq_occupancy_sum(pmq_occupancy_sum),
            s_pmq_occupancy_max(pmq_occupancy_max)
        {
            init_dram_params(m_cfg.m_dram);
            reset();
        }

        ~PerBankStructures() {
            m_pmq.clear();
            m_ssq.clear();
            m_SHQ.clear();
        }

        void on_request(const Request& req) {
            auto it = m_handlertable.find(req.command);
            if (it != m_handlertable.end()) {
                it->second.handler(req);
            }
        }

        void init_dram_params(IDRAM* dram) {
            CommandHandler handlers[] = {
                {std::string("ACT"),   std::bind(&PerBankStructures::process_act, this, std::placeholders::_1)},
                {std::string("RFMab"), std::bind(&PerBankStructures::process_rfm, this, std::placeholders::_1)},
                {std::string("RFMsb"), std::bind(&PerBankStructures::process_proactive_rfm, this, std::placeholders::_1)},
                {std::string("RFMpb"), std::bind(&PerBankStructures::process_proactive_rfm, this, std::placeholders::_1)},
                {std::string("REFab"), std::bind(&PerBankStructures::process_targeted_ref, this, std::placeholders::_1)}
            };
            for (auto& h : handlers) {
                if (!dram->m_commands.contains(h.cmd_name)) {
                    std::cout << "[PrISM] Command " << h.cmd_name
                              << " does not exist." << std::endl;
                    std::exit(EXIT_FAILURE);
                }
                m_handlertable[dram->m_commands(h.cmd_name)] = h;
            }
        }

        bool has_pmq_threshold_alert() const {
            for (const auto& e : m_pmq) {
                if (e.act_count > m_pmq_threshold) return true;
            }
            return false;
        }

        bool has_pmq_capacity_alert() const {
            // Hard cap: alert fires the moment PMQ reaches capacity.
            return static_cast<int>(m_pmq.size()) >= m_pmq_capacity;
        }

        bool is_local_abo_needed() const { return m_local_abo_needed; }

        int get_pmq_occupancy() const {
            return static_cast<int>(m_pmq.size());
        }

        void reset() {
            m_local_abo_needed = false;
            m_current_act_count = 0;
            m_num_ref = 0;
            m_pmq.clear();
            m_pmq_insertion_seq = 0;
            m_ssq.clear();
            m_SHQ.clear();
            reset_sampled_act_slots();
        }

    private:
        // ----------- types ---------------------------------------------------
        struct CommandHandler {
            std::string cmd_name;
            std::function<void(const Request&)> handler;
        };

        struct PMQEntry {
            int row;
            int act_count;
            bool threshold_crossing_counted;
            bool from_default;
            bool from_intersection;
            uint64_t insertion_seq;
        };

        // SSQ entries are kept across window boundaries (circular). An entry
        // is "live" as long as `consumed == false`. After every mitigation we
        // sweep out consumed entries so the queue does not grow unboundedly.
        //
        //   wants_pmq=false, consumed=false -> non-intersecting sample,
        //                                      waiting for window close
        //   wants_pmq=true,  consumed=false -> pending PMQ insertion
        //                                      (intersection or deferred default)
        //   consumed=true                   -> serviced (in PMQ or in SHQ),
        //                                      eligible for cleanup
        struct SSQEntry {
            int  row;
            bool wants_pmq;
            bool consumed;
        };

        enum class MitigationSource {
            None,
            PMQ_ABO,
            PMQ_Opportunistic,
            SSQDefaultFallback
        };

        // ----------- members -------------------------------------------------
        DeviceConfig& m_cfg;
        bool m_local_abo_needed = false;

        int m_max_acts_per_mitigation;
        int m_sampled_rows_per_mitigation;
        int m_shq_length;
        int m_shq_capacity;
        int m_pmq_threshold;
        int m_pmq_capacity;
        int m_ssq_capacity;

        // Sampling state for the CURRENT window. `m_sampled_act_slots` are the
        // pre-picked slot indices; once `m_current_act_count` reaches W we
        // close the window.
        std::unordered_set<int> m_sampled_act_slots;
        int m_current_act_count = 0;
        int m_sample_epoch_counter = 0;

        // SSQ kept across windows
        std::deque<SSQEntry> m_ssq;

        // SHQ FIFO of recent sampled-but-unselected rows
        std::deque<int> m_SHQ;

        // PMQ
        std::deque<PMQEntry> m_pmq;
        uint64_t m_pmq_insertion_seq = 0;

        // RNG
        std::mt19937 gen;

        uint32_t m_targeted_ref_frequency;
        uint64_t m_num_ref = 0;

        std::unordered_map<int, CommandHandler> m_handlertable;

        bool m_debug = false;
        int  m_bank_id = -1;

        // ----------- stat refs -----------------------------------------------
        uint64_t& s_num_targeted_ref;
        uint64_t& s_num_total_mitigations;
        uint64_t& s_num_pmq_mitigations_abo;
        uint64_t& s_num_pmq_mitigations_opportunistic;
        uint64_t& s_num_sampled_candidate_mitigations;
        uint64_t& s_num_wasted_abo_slots;
        uint64_t& s_num_pmq_inserts_intersection_immediate;
        uint64_t& s_num_pmq_inserts_intersection_deferred;
        uint64_t& s_num_pmq_inserts_default;
        uint64_t& s_num_pmq_merges;
        uint64_t& s_num_pmq_threshold_crossings;
        uint64_t& s_ssq_pending_intersection_max;
        uint64_t& s_ssq_max_occupancy;
        uint64_t& s_ssq_drain_to_pmq;
        uint64_t& s_pmq_occupancy_samples;
        uint64_t& s_pmq_occupancy_sum;
        uint64_t& s_pmq_occupancy_max;

        // ----------- helpers -------------------------------------------------
        void reset_sampled_act_slots() {
            m_sampled_act_slots.clear();
            if (m_sampled_rows_per_mitigation > m_max_acts_per_mitigation) {
                std::cerr << "[PrISM][CRITICAL] R > W: R="
                          << m_sampled_rows_per_mitigation
                          << " W=" << m_max_acts_per_mitigation << "\n";
                std::exit(EXIT_FAILURE);
            }
            std::uniform_int_distribution<int> d(0, m_max_acts_per_mitigation - 1);
            int seed = 42 + m_bank_id * 1000 + m_sample_epoch_counter++;
            gen.seed(seed);
            while ((int)m_sampled_act_slots.size() < m_sampled_rows_per_mitigation) {
                m_sampled_act_slots.insert(d(gen));
            }
        }

        bool shq_contains(int row) const {
            if (row < 0) return false;
            for (int s : m_SHQ) if (s == row) return true;
            return false;
        }

        bool pmq_contains(int row) const {
            for (const auto& e : m_pmq) if (e.row == row) return true;
            return false;
        }

        // Has `row` already been queued into the SSQ this window AND not yet
        // consumed?
        bool ssq_has_live(int row) const {
            for (const auto& e : m_ssq) {
                if (!e.consumed && e.row == row) return true;
            }
            return false;
        }

        int count_ssq_pending_intersection() const {
            int n = 0;
            for (const auto& e : m_ssq) {
                if (!e.consumed && e.wants_pmq) n++;
            }
            return n;
        }

// SSQ capacity is a HARD security bound
        void check_ssq_capacity() {
            int pending = 0;
            int live    = 0;
            for (const auto& e : m_ssq) {
                if (!e.consumed) {
                    live++;
                    if (e.wants_pmq) pending++;
                }
            }
            s_ssq_max_occupancy = std::max(s_ssq_max_occupancy, (uint64_t)live);
            s_ssq_pending_intersection_max =
                std::max(s_ssq_pending_intersection_max, (uint64_t)pending);

            if (live > m_ssq_capacity) {
                // overflow — Eq. (1) bound violated
                std::cerr << "[PrISM] SSQ overflow at "
                          << " bank=" << m_bank_id
                          << " live=" << live
                          << " capacity=" << m_ssq_capacity
                          << " pending=" << pending
                          << " — Eq. (1) bound violated; SSQ sizing must be revisited.\n";
                std::exit(EXIT_FAILURE);
            }
        }

        void cleanup_consumed_ssq() {
            auto new_end = std::remove_if(
                m_ssq.begin(), m_ssq.end(),
                [](const SSQEntry& e) { return e.consumed; });
            m_ssq.erase(new_end, m_ssq.end());
        }

        void refresh_local_abo_flag() {
            m_local_abo_needed =
                has_pmq_threshold_alert() || has_pmq_capacity_alert();
        }

        void record_pmq_occupancy() {
            const uint64_t sz = static_cast<uint64_t>(m_pmq.size());
            s_pmq_occupancy_samples++;
            s_pmq_occupancy_sum += sz;
            s_pmq_occupancy_max = std::max(s_pmq_occupancy_max, sz);
        }

        bool enqueue_or_merge_pmq(int row, bool from_default,
                                  bool from_intersection) {
            if (row < 0) return false;

            // Merge if already present.
            for (auto& e : m_pmq) {
                if (e.row == row) {
                    e.from_default      = e.from_default      || from_default;
                    e.from_intersection = e.from_intersection || from_intersection;
                    s_num_pmq_merges++;
                    refresh_local_abo_flag();
                    record_pmq_occupancy();
                    return true;
                }
            }

            // HARD cap.
            if (static_cast<int>(m_pmq.size()) >= m_pmq_capacity) {
                return false;
            }

            m_pmq.push_back({
                row, 0, false, from_default, from_intersection,
                m_pmq_insertion_seq++
            });

            if (from_default)      s_num_pmq_inserts_default++;
            if (from_intersection) s_num_pmq_inserts_intersection_immediate++;

            refresh_local_abo_flag();
            record_pmq_occupancy();
            return true;
        }

        bool increment_pmq_counter_on_act(int row) {
            for (auto& e : m_pmq) {
                if (e.row == row) {
                    e.act_count++;
                    if (!e.threshold_crossing_counted &&
                        e.act_count > m_pmq_threshold) {
                        e.threshold_crossing_counted = true;
                        s_num_pmq_threshold_crossings++;
                    }
                    refresh_local_abo_flag();
                    return true;
                }
            }
            return false;
        }

        // Pop the PMQ entry with the highest activation count (preferring those
        // that have crossed T_PMQ).
        int pop_highest_count_pmq() {
            if (m_pmq.empty()) return -1;
            auto best_it = m_pmq.end();
            int  best_count = -1;
            for (auto it = m_pmq.begin(); it != m_pmq.end(); ++it) {
                if (it->act_count > m_pmq_threshold &&
                    it->act_count > best_count) {
                    best_it = it;
                    best_count = it->act_count;
                }
            }
            if (best_it == m_pmq.end()) {
                for (auto it = m_pmq.begin(); it != m_pmq.end(); ++it) {
                    if (it->act_count > best_count) {
                        best_it = it;
                        best_count = it->act_count;
                    }
                }
            }
            int row = best_it->row;
            m_pmq.erase(best_it);
            record_pmq_occupancy();
            return row;
        }

        // After a PMQ slot frees, try to drain the oldest pending entry from
        // the SSQ into the PMQ.
        void drain_pending_ssq_to_pmq() {
            if (static_cast<int>(m_pmq.size()) >= m_pmq_capacity) return;
            for (auto& e : m_ssq) {
                if (e.consumed || !e.wants_pmq) continue;
                // Skip if a duplicate already lives in PMQ.
                if (pmq_contains(e.row)) {
                    e.consumed = true;
                    continue;
                }
                bool ok = enqueue_or_merge_pmq(e.row,
                                               /*from_default=*/false,
                                               /*from_intersection=*/true);
                if (ok) {
                    e.consumed = true;
                    s_num_pmq_inserts_intersection_deferred++;
                    s_ssq_drain_to_pmq++;
                }
                // After at most one successful drain we exit. Stalled
                // mitigations will keep calling us until the SSQ is empty.
                return;
            }
        }

        // ============== PROCESS ACT =========================================
        void process_act(const Request& req) {
            int row = req.addr_vec[m_cfg.m_row_level];

            // 1) Update PMQ counters for any row already pending mitigation.
            increment_pmq_counter_on_act(row);

            // 2) If this ACT lands on a pre-picked sampled slot, run the
            //    intersection check immediately and try to enqueue the PMQ.
            const bool sampled =
                m_sampled_act_slots.find(m_current_act_count) !=
                m_sampled_act_slots.end();

            if (sampled && !ssq_has_live(row)) {
                const bool intersects = shq_contains(row);
                SSQEntry e{row, intersects, false};
                m_ssq.push_back(e);

                if (intersects) {
                    if (pmq_contains(row)) {
                        // Already pending mitigation; nothing new to do.
                        // Mark consumed to avoid pushing this row to the SHQ
                        // at window close.
                        m_ssq.back().consumed = true;
                    } else {
                        bool ok = enqueue_or_merge_pmq(row,
                                                       /*from_default=*/false,
                                                       /*from_intersection=*/true);
                        if (ok) {
                            m_ssq.back().consumed = true;
                        }
                        // else: stays pending; drained by drain_pending_ssq_to_pmq
                    }
                }
            }

            m_current_act_count++;

            check_ssq_capacity();

            if (m_current_act_count >= m_max_acts_per_mitigation) {
                finalize_window();
            }
        }

        // Window close: pick the default mitigation winner, push the rest of
        // the non-intersecting samples to the SHQ, pad with placeholders.
        void finalize_window() {
            // ----- Phase A: pick default winner via reservoir sample over
            //                non-intersecting, not-yet-consumed SSQ entries.
            int default_row = -1;
            int default_idx = -1;
            int count = 0;
            for (size_t i = 0; i < m_ssq.size(); i++) {
                const auto& e = m_ssq[i];
                if (e.consumed || e.wants_pmq) continue;
                count++;
                std::uniform_int_distribution<int> d(0, count - 1);
                if (d(gen) == 0) {
                    default_row = e.row;
                    default_idx = static_cast<int>(i);
                }
            }

            // ----- Phase B: try to enqueue default into the PMQ.
            if (default_idx >= 0) {
                bool ok = enqueue_or_merge_pmq(default_row,
                                               /*from_default=*/true,
                                               /*from_intersection=*/false);
                if (ok) {
                    m_ssq[default_idx].consumed = true;
                } else {
                    // PMQ full: defer this default like an intersection.
                    m_ssq[default_idx].wants_pmq = true;
                }
            }

            // ----- Phase C: push remaining non-intersecting, non-consumed,
            //                non-deferred SSQ entries to the SHQ (deduped
            //                within the window).
            int pushed = 0;
            std::unordered_set<int> pushed_this_window;
            for (auto& e : m_ssq) {
                if (e.consumed || e.wants_pmq) continue;
                if (pushed_this_window.count(e.row)) {
                    e.consumed = true; // dedup within window
                    continue;
                }
                m_SHQ.push_back(e.row);
                pushed_this_window.insert(e.row);
                e.consumed = true;
                pushed++;
            }

            // ----- Phase D: pad SHQ with placeholders to keep L deterministic.
            while (pushed < m_sampled_rows_per_mitigation - 1) {
                m_SHQ.push_back(-1);
                pushed++;
            }

            // ----- Phase E: trim SHQ to capacity.
            while (static_cast<int>(m_SHQ.size()) > m_shq_capacity) {
                m_SHQ.pop_front();
            }

            // ----- Phase F: clean up consumed SSQ entries.
            cleanup_consumed_ssq();

            // ----- Phase G: refresh ABO flag and prep next window.
            refresh_local_abo_flag();
            record_pmq_occupancy();
            check_ssq_capacity();

            m_current_act_count = 0;
            reset_sampled_act_slots();

            if (m_debug) {
                std::cout << "[PrISM-WINDOW][Bank " << m_bank_id
                          << "] close: default=" << default_row
                          << " pushed_to_shq=" << pushed
                          << " ssq_live=" << m_ssq.size()
                          << " pmq=" << m_pmq.size() << "\n";
            }
        }

        // mode 0: targeted REF, mode 1: ABO-RFM, mode 2: proactive RFM
        void process_mitigation(int mode) {
            int selected_row = -1;
            MitigationSource source = MitigationSource::None;

            if (!m_pmq.empty()) {
                selected_row = pop_highest_count_pmq();
                if (mode == 1) {
                    source = MitigationSource::PMQ_ABO;
                    s_num_pmq_mitigations_abo++;
                } else {
                    source = MitigationSource::PMQ_Opportunistic;
                    s_num_pmq_mitigations_opportunistic++;
                }
            } else {
                // Opportunistic fallback: any unconsumed SSQ entry from the
                // current window. This does not weaken security (these were
                // going to be mitigation candidates anyway) but improves
                // performance under benign workloads.
                int best_idx = -1;
                int count = 0;
                for (size_t i = 0; i < m_ssq.size(); i++) {
                    const auto& e = m_ssq[i];
                    if (e.consumed) continue;
                    count++;
                    std::uniform_int_distribution<int> d(0, count - 1);
                    if (d(gen) == 0) best_idx = static_cast<int>(i);
                }
                if (best_idx >= 0) {
                    selected_row = m_ssq[best_idx].row;
                    m_ssq[best_idx].consumed = true;
                    source = MitigationSource::SSQDefaultFallback;
                    s_num_sampled_candidate_mitigations++;
                } else if (mode == 1) {
                    s_num_wasted_abo_slots++;
                }
            }

            if (selected_row != -1) {
                s_num_total_mitigations++;
            }

            // After freeing a PMQ slot (mode 1/2 may have popped from PMQ
            // above), try to drain a pending intersection.
            drain_pending_ssq_to_pmq();

            refresh_local_abo_flag();
        }

        void process_targeted_ref(const Request& /*req*/) {
            if (m_targeted_ref_frequency == 0) return;
            m_num_ref++;
            if (m_num_ref % m_targeted_ref_frequency != 0) return;

            process_mitigation(0);

            if (m_bank_id == 0) s_num_targeted_ref += m_cfg.m_num_ranks;
        }

        void process_rfm(const Request& /*req*/)           { process_mitigation(1); }
        void process_proactive_rfm(const Request& /*req*/) { process_mitigation(2); }
    };  // class PerBankStructures
};      // class PRISM

}       // namespace Ramulator
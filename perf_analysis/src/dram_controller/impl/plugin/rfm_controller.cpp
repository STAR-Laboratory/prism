#include "base/base.h"
#include "dram_controller/controller.h"
#include "dram_controller/plugin.h"

namespace Ramulator {

class RFMController : public IControllerPlugin, public Implementation {
    RAMULATOR_REGISTER_IMPLEMENTATION(IControllerPlugin, RFMController, "RFMController", "RFMController Implementation")

private:
    IDRAM* m_dram = nullptr;

    // Per-bank RAA (Rolling Accumulation of ACTs) counters.
    std::vector<int> m_bank_ctrs;

    Clk_t m_clk = 0;

    // RFM request id resolved at setup() based on m_rfm_type.
    int m_rfm_req_id = -1;

    int m_rank_level      = -1;
    int m_bank_level      = -1;
    int m_bankgroup_level = -1;
    int m_row_level       = -1;
    int m_col_level       = -1;

    int m_num_ranks               = -1;
    int m_num_bankgroups          = -1;
    int m_num_banks_per_bankgroup = -1;
    int m_num_banks_per_rank      = -1;
    int m_num_rows_per_bank       = -1;
    int m_num_cls                 = -1;

    // Configuration parameters.
    int  m_bat                       = -1;
    int  m_rfm_type                  = -1; // 0: RFMab, 1: RFMsb, 2: RFMpb
    int  m_targeted_ref_freq         =  0;
    bool m_enable_early_counter_reset = true;
    int  m_max_rfm_postponement      =  2;
    bool m_debug                     = false;

    // Derived: RAA threshold at which RFM MUST be issued.
    int m_max_raa_cnt = -1;

    // REFab-based RAA credit bookkeeping.
    std::vector<int> m_refab_cnt_per_rank;

    // Stats
    uint64_t s_rfm_counter         = 0;
    uint64_t s_early_counter_reset = 0;
    uint64_t s_skipped_rfm         = 0; // reserved; not used in current design

    // Postponement accounting: a bank counter crossing a `bat` boundary
    // represents one "RFM opportunity" -- a moment when an RFM could
    // legally have issued. If the counter is still below m_max_raa_cnt
    // after the crossing, the RFM is deferred (postponed); otherwise it
    // is forced. With max_rfm_postponement = k, we expect roughly
    //   num_rfm_postponed : num_rfm  ==  k : 1
    // in steady state.
    uint64_t s_rfm_postponed = 0;

   
    uint64_t s_act_count_seen = 0;
    uint64_t s_max_bank_ctr   = 0;

public:
    void init() override {
        m_bat                        = param<int>("bat").default_val(75);
        m_debug                      = param<bool>("debug").default_val(false);
        m_rfm_type                   = param<int>("rfm_type").default_val(1);
        m_targeted_ref_freq          = param<int>("targeted_ref_frequency").default_val(1);
        m_enable_early_counter_reset = param<bool>("enable_early_counter_reset").default_val(true);
        m_max_rfm_postponement       = param<int>("max_rfm_postponement").default_val(2);
    }

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
        m_ctrl = cast_parent<IDRAMController>();
        m_dram = m_ctrl->m_dram;

        if (!m_dram->m_requests.contains("rfm")) {
            std::cout << "[Ramulator::RFMController] [CRITICAL ERROR] DRAM Device does not support request: RFMab" << std::endl;
            exit(0);
        } else if (!m_dram->m_requests.contains("same-bank-rfm")) {
            std::cout << "[Ramulator::RFMController] [CRITICAL ERROR] DRAM Device does not support request: RFMsb" << std::endl;
            exit(0);
        } else if (m_rfm_type == 2 && !m_dram->m_requests.contains("per-bank-rfm")) {
            std::cout << "[Ramulator::RFMController] [CRITICAL ERROR] DRAM Device does not support request: RFMpb" << std::endl;
            exit(0);
        }

        switch (m_rfm_type) {
            case 0: m_rfm_req_id = m_dram->m_requests("rfm");           break;
            case 1: m_rfm_req_id = m_dram->m_requests("same-bank-rfm"); break;
            case 2: m_rfm_req_id = m_dram->m_requests("per-bank-rfm");  break;
            default:
                std::cout << "[Ramulator::RFMController] [CRITICAL ERROR] Invalid rfm_type: " << m_rfm_type << std::endl;
                exit(0);
        }

        m_rank_level      = m_dram->m_levels("rank");
        m_bank_level      = m_dram->m_levels("bank");
        m_bankgroup_level = m_dram->m_levels("bankgroup");
        m_row_level       = m_dram->m_levels("row");
        m_col_level       = m_dram->m_levels("column");

        m_num_ranks               = m_dram->get_level_size("rank");
        m_num_bankgroups          = m_dram->get_level_size("bankgroup");
        m_num_banks_per_bankgroup = m_dram->get_level_size("bankgroup") < 0 ? 0 : m_dram->get_level_size("bank");
        m_num_banks_per_rank      = m_dram->get_level_size("bankgroup") < 0
                                    ? m_dram->get_level_size("bank")
                                    : m_dram->get_level_size("bankgroup") * m_dram->get_level_size("bank");
        m_num_rows_per_bank       = m_dram->get_level_size("row");
        m_num_cls                 = m_dram->get_level_size("column") / 8;

        m_bank_ctrs.assign(m_num_ranks * m_num_banks_per_rank, 0);
        m_refab_cnt_per_rank.assign(m_num_ranks, 0);

        m_max_raa_cnt = (m_max_rfm_postponement + 1) * m_bat;

        register_stat(s_rfm_counter).name("num_rfm");
        register_stat(s_skipped_rfm).name("num_skipped_rfm");
        register_stat(s_rfm_postponed).name("num_rfm_postponed");
        register_stat(s_early_counter_reset).name("num_early_counter_reset");
        register_stat(s_act_count_seen).name("rfm_act_count_seen");
        register_stat(s_max_bank_ctr).name("rfm_max_bank_ctr_observed");

        std::printf("[RFMController] BAT (RFMTH): %d, Max RFM postponement: %d, Max RAA cnt: %d, Targeted REF Freq: %d\n",
                    m_bat, m_max_rfm_postponement, m_max_raa_cnt, m_targeted_ref_freq);
    }

    int calc_flat_bank_id(ReqBuffer::iterator& req_it) {
        int flat_bank_id = req_it->addr_vec[m_bank_level];
        int accumulated_dimension = 1;
        for (int i = m_bank_level - 1; i >= m_rank_level; i--) {
            accumulated_dimension *= m_dram->m_organization.count[i + 1];
            flat_bank_id += req_it->addr_vec[i] * accumulated_dimension;
        }
        return flat_bank_id;
    }

    // Credit per-bank RAA counters on REFab.
    //
    // Every m_targeted_ref_freq-th REFab on the rank decrements all banks of
    // that rank by m_bat (floored at 0).
    void credit_raa_on_refab(ReqBuffer::iterator& req_it) {
        const int rank = req_it->addr_vec[m_rank_level];
        m_refab_cnt_per_rank[rank]++;

        if ((m_refab_cnt_per_rank[rank] % m_targeted_ref_freq) != 0) {
            return;
        }

        for (int i = 0; i < m_num_banks_per_rank; i++) {
            const int flat_bank_id = rank * m_num_banks_per_rank + i;
            if (m_bank_ctrs[flat_bank_id] > 0) {
                if (m_bank_ctrs[flat_bank_id] >= m_bat) {
                    m_bank_ctrs[flat_bank_id] -= m_bat;
                } else {
                    m_bank_ctrs[flat_bank_id] = 0;
                }
            }
            if (m_debug) {
                std::printf("[RFMController] REF-credit @ %ld: rank=%d flat=%d new_ctr=%d\n",
                            m_clk, rank, flat_bank_id, m_bank_ctrs[flat_bank_id]);
            }
        }
    }

    // Decrement m_bank_ctrs[id] by m_bat (floor at 0). Used after enqueueing
    // an RFM that services this bank.
    inline void subtract_bat(int flat_bank_id) {
        if (m_bank_ctrs[flat_bank_id] > 0) {
            if (m_bank_ctrs[flat_bank_id] >= m_bat) {
                m_bank_ctrs[flat_bank_id] -= m_bat;
            } else {
                m_bank_ctrs[flat_bank_id] = 0;
            }
        }
    }

    void update(bool request_found, ReqBuffer::iterator& req_it) override {
        m_clk++;

        if (!request_found) {
            return;
        }

        auto& req       = *req_it;
        auto& req_meta  = m_dram->m_command_meta(req.command);
        auto& req_scope = m_dram->m_command_scopes(req.command);

        // Targeted Refresh -> reducing RAA.
        if (m_targeted_ref_freq != 0 && req.command == m_dram->m_commands("REFab")) {
            credit_raa_on_refab(req_it);
        }

        // Only process ACT commands (row-opening).
        if (!(req_meta.is_opening && req_scope == m_row_level)) {
            return;
        }

        int flat_bank_id = calc_flat_bank_id(req_it);
        m_bank_ctrs[flat_bank_id]++;

        s_act_count_seen++;
        if ((uint64_t)m_bank_ctrs[flat_bank_id] > s_max_bank_ctr) {
            s_max_bank_ctr = m_bank_ctrs[flat_bank_id];
        }

        if (m_bank_ctrs[flat_bank_id] > 0 &&
            (m_bank_ctrs[flat_bank_id] % m_bat) == 0 &&
            m_bank_ctrs[flat_bank_id] < m_max_raa_cnt) {
            s_rfm_postponed++;
        }

        if (m_debug) {
            std::cout << "Increment BAC @ " << m_clk << std::endl;
            std::cout << "Rank     : " << req_it->addr_vec[m_rank_level] << std::endl;
            std::cout << "BankGroup: " << req_it->addr_vec[m_bankgroup_level] << std::endl;
            std::cout << "Bank     : " << req_it->addr_vec[m_bank_level] << std::endl;
            std::cout << "Flat Bank: " << flat_bank_id << std::endl;
            std::cout << "Counter  : " << m_bank_ctrs[flat_bank_id] << std::endl;
            std::cout << "Row      : " << req_it->addr_vec[m_row_level] << std::endl;
        }

        // --- RFM issuing condition (with postponement) ---
        // RFM is forced only when RAA reaches the max postponement limit.
        if (m_bank_ctrs[flat_bank_id] < m_max_raa_cnt) {
            return;
        }

        const int trig_rank = req.addr_vec[m_rank_level];
        const int trig_bank = req.addr_vec[m_bank_level];

        switch (m_rfm_type) {
            case 0: {   // RFMab -- refreshes all banks in the rank.
                Request rfm(req.addr_vec, m_rfm_req_id);
                rfm.addr_vec[m_bankgroup_level] = -1;
                rfm.addr_vec[m_bank_level]      = -1;

                if (!m_ctrl->priority_send(rfm)) {
                    std::cout << "[Ramulator::RFMController] [CRITICAL ERROR] Could not send request: RFMab" << std::endl;
                    exit(0);
                }
                s_rfm_counter++;

                if (m_enable_early_counter_reset) {
                    // RFMab services all banks in the rank.
                    for (int i = 0; i < m_num_banks_per_rank; i++) {
                        int id = trig_rank * m_num_banks_per_rank + i;
                        subtract_bat(id);
                    }
                } else {
                    subtract_bat(flat_bank_id);
                }
                break;
            }

            case 1: {   // RFMsb -- refreshes one bank across all bank groups
                        //          in the rank.
                Request rfm(req.addr_vec, m_rfm_req_id);
                rfm.addr_vec[m_bankgroup_level] = -1;

                if (!m_ctrl->priority_send(rfm)) {
                    std::cout << "[Ramulator::RFMController] [CRITICAL ERROR] Could not send request: RFMsb" << std::endl;
                    exit(0);
                }
                s_rfm_counter++;

                if (m_enable_early_counter_reset) {
                    // RFMsb services one bank per bank group on the rank.
                    for (size_t j = 0; j < (size_t)m_num_bankgroups; j++) {
                        int id = trig_bank
                               + j * m_num_banks_per_bankgroup
                               + trig_rank * m_num_banks_per_rank;
                        subtract_bat(id);
                    }
                } else {
                    subtract_bat(flat_bank_id);
                }
                break;
            }

            case 2: {   // RFMpb -- refreshes a single bank (not in JEDEC spec).
                Request rfm(req.addr_vec, m_rfm_req_id);

                if (!m_ctrl->priority_send(rfm)) {
                    std::cout << "[Ramulator::RFMController] [CRITICAL ERROR] Could not send request: RFMpb" << std::endl;
                    exit(0);
                }
                s_rfm_counter++;
                subtract_bat(flat_bank_id);
                break;
            }
        }
    }
};

}   // namespace Ramulator
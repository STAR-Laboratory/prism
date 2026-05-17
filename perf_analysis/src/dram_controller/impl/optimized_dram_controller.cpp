#include "dram_controller/bh_controller.h"
#include "memory_system/memory_system.h"
#include "frontend/frontend.h"
#include "frontend/impl/processor/bhO3/bhllc.h"
#include "frontend/impl/processor/bhO3/bhO3.h"

namespace Ramulator {

DECLARE_DEBUG_FLAG(DBHCTRL);
ENABLE_DEBUG_FLAG(DBHCTRL);

class OPTDRAMController final : public IBHDRAMController, public Implementation {
    RAMULATOR_REGISTER_IMPLEMENTATION(IBHDRAMController, OPTDRAMController, "OPTDRAMController", "Optimized DRAM controller.");

private:
    Logger_t m_logger;
    std::deque<Request> pending;          // Reads waiting on RL callback.
    BHO3LLC* m_llc;

    ReqBuffer m_active_buffer;            // In-progress requests (top priority).
    ReqBuffer m_priority_buffer;          // Maintenance (REF, RFMab, RFMsb, RFMpb).
    ReqBuffer m_read_buffer;
    ReqBuffer m_write_buffer;

    int m_rank_addr_idx      = -1;
    int m_bankgroup_addr_idx = -1;
    int m_bank_addr_idx      = -1;
    int m_row_addr_idx       = -1;

    float m_wr_low_watermark;
    float m_wr_high_watermark;
    bool  m_is_write_mode = false;

    // ---- Standard accounting ------------------------------------------------
    std::vector<int> s_core_row_hits;
    std::vector<int> s_core_row_misses;
    std::vector<int> s_core_row_conflicts;

    uint64_t s_num_read_reqs    = 0;
    uint64_t s_num_write_reqs   = 0;
    uint64_t s_num_other_reqs   = 0;
    uint64_t s_num_refresh_reqs = 0;
    double   s_num_ref_windows  = 0;
    uint64_t s_num_rfm_reqs     = 0;

    uint64_t s_queue_len          = 0;
    uint64_t s_read_queue_len     = 0;
    uint64_t s_write_queue_len    = 0;
    uint64_t s_priority_queue_len = 0;

    double s_queue_len_avg          = 0;
    double s_read_queue_len_avg     = 0;
    double s_write_queue_len_avg    = 0;
    double s_priority_queue_len_avg = 0;

    uint64_t s_read_latency     = 0;
    double   s_read_latency_avg = 0;

    uint64_t s_num_row_hits      = 0;
    uint64_t s_num_row_misses    = 0;
    uint64_t s_num_row_conflicts = 0;

    // Counter for demand requests deferred due to pending maintenance.
    uint64_t s_num_demand_deferred_pending_maint = 0;

public:
    void init() override {
        m_wr_low_watermark  = param<float>("wr_low_watermark").desc("Threshold for switching back to read mode.").default_val(0.2f);
        m_wr_high_watermark = param<float>("wr_high_watermark").desc("Threshold for switching to write mode.").default_val(0.8f);

        m_scheduler = create_child_ifce<IBHScheduler>();
        m_refresh   = create_child_ifce<IRefreshManager>();
        m_rowpolicy = create_child_ifce<IRowPolicy>();

        if (m_config["plugins"]) {
            YAML::Node plugin_configs = m_config["plugins"];
            for (YAML::iterator it = plugin_configs.begin(); it != plugin_configs.end(); ++it) {
                m_plugins.push_back(create_child_ifce<IControllerPlugin>(*it));
            }
        }
    };

    void setup(IFrontEnd* frontend, IMemorySystem* memory_system) override {
        m_llc                = static_cast<BHO3*>(frontend)->get_llc();
        m_dram               = memory_system->get_ifce<IDRAM>();
        m_rank_addr_idx      = m_dram->m_levels("rank");
        m_bankgroup_addr_idx = m_dram->m_levels("bankgroup");
        m_bank_addr_idx      = m_dram->m_levels("bank");
        m_row_addr_idx       = m_dram->m_levels("row");

        m_logger = Logging::create_logger(fmt::format("DBHCTRL_{}", m_channel_id));

        int num_cores = static_cast<BHO3*>(frontend)->get_num_cores();
        s_core_row_hits.resize(num_cores);
        s_core_row_misses.resize(num_cores);
        s_core_row_conflicts.resize(num_cores);

        for (int i = 0; i < num_cores; i++) {
            register_stat(s_core_row_hits[i]).name("controller_core_row_hits_{}_ch{}", i, m_channel_id);
            register_stat(s_core_row_misses[i]).name("controller_core_row_misses_{}_ch{}", i, m_channel_id);
            register_stat(s_core_row_conflicts[i]).name("controller_core_row_conflicts_{}_ch{}", i, m_channel_id);
        }

        m_active_buffer.max_size   = INT_MAX;
        m_priority_buffer.max_size = INT_MAX;

        register_stat(s_num_row_hits).name("controller{}_num_row_hits", m_channel_id);
        register_stat(s_num_row_misses).name("controller{}_num_row_misses", m_channel_id);
        register_stat(s_num_row_conflicts).name("controller{}_num_row_conflicts", m_channel_id);

        register_stat(s_num_read_reqs).name("controller{}_num_read_reqs", m_channel_id);
        register_stat(s_num_write_reqs).name("controller{}_num_write_reqs", m_channel_id);
        register_stat(s_num_other_reqs).name("controller{}_num_other_reqs", m_channel_id);

        register_stat(s_num_refresh_reqs).name("controller{}_num_refresh_reqs", m_channel_id);
        register_stat(s_num_ref_windows).name("controller{}_num_ref_windows", m_channel_id);

        register_stat(s_num_rfm_reqs).name("controller{}_num_rfm_reqs", m_channel_id);

        register_stat(s_read_latency).name("controller{}_read_latency", m_channel_id);
        register_stat(s_read_latency_avg).name("controller{}_read_latency_avg", m_channel_id);

        register_stat(s_queue_len).name("controller{}_queue_len", m_channel_id);
        register_stat(s_read_queue_len).name("controller{}_read_queue_len", m_channel_id);
        register_stat(s_write_queue_len).name("controller{}_write_queue_len", m_channel_id);
        register_stat(s_priority_queue_len).name("controller{}_priority_queue_len", m_channel_id);

        register_stat(s_queue_len_avg).name("controller{}_queue_len_avg", m_channel_id);
        register_stat(s_read_queue_len_avg).name("controller{}_read_queue_len_avg", m_channel_id);
        register_stat(s_write_queue_len_avg).name("controller{}_write_queue_len_avg", m_channel_id);
        register_stat(s_priority_queue_len_avg).name("controller{}_priority_queue_len_avg", m_channel_id);

        register_stat(s_num_demand_deferred_pending_maint).name("controller{}_num_demand_deferred_pending_maint", m_channel_id);
    };

    bool send(Request& req) override {
        req.final_command = m_dram->m_request_translations(req.type_id);

        // Forward existing write requests to incoming read requests.
        if (req.type_id == Request::Type::Read) {
            auto compare_addr = [req](const Request& wreq) { return wreq.addr == req.addr; };
            if (std::find_if(m_write_buffer.begin(), m_write_buffer.end(), compare_addr) != m_write_buffer.end()) {
                req.depart = m_clk + 1;
                pending.push_back(req);
                return true;
            }
        }

        bool is_success = false;
        req.arrive = m_clk;
        if        (req.type_id == Request::Type::Read) {
            is_success = m_read_buffer.enqueue(req);
        } else if (req.type_id == Request::Type::Write) {
            is_success = m_write_buffer.enqueue(req);
        } else {
            throw std::runtime_error("Invalid request type!");
        }
        if (!is_success) { req.arrive = -1; return false; }
        return true;
    };

    bool priority_send(Request& req) override {
        req.final_command = m_dram->m_request_translations(req.type_id);
        return m_priority_buffer.enqueue(req);
    }

    // Demand-vs-pending-maintenance conflict check.
    //
    // Returns true if a pending maintenance command (REFab/RFMab/RFMsb/RFMpb)
    // in the priority buffer would conflict with a demand to `demand_addr_vec`.
    //
    //
    // Scope rules per JEDEC:
    //   - REFab  : (bg=-1, bank=-1)        -> block whole rank
    //   - RFMab  : (bg=-1, bank=-1)        -> block whole rank
    //   - RFMsb  : (bg=-1, bank=real_bank) -> block same rank + same bank index
    //                                         across all bank groups
    //   - RFMpb  : (bg=real, bank=real)    -> block exact (rank, bg, bank)
    bool is_demand_blocked_by_pending_maintenance(const std::vector<int>& demand_addr_vec) {
        const int demand_rank = demand_addr_vec[m_rank_addr_idx];
        const int demand_bg   = demand_addr_vec[m_bankgroup_addr_idx];
        const int demand_bank = demand_addr_vec[m_bank_addr_idx];
        if (demand_rank < 0) return false;

        const int refab_id = m_dram->m_commands("REFab");
        const int rfmab_id = m_dram->m_commands("RFMab");
        const int rfmsb_id = m_dram->m_commands("RFMsb");
        const int rfmpb_id = m_dram->m_commands("RFMpb");

        for (auto it = m_priority_buffer.begin(); it != m_priority_buffer.end(); ++it) {
            const int fc = it->final_command;
            if (fc != refab_id && fc != rfmab_id && fc != rfmsb_id && fc != rfmpb_id) {
                continue;
            }

            const int maint_rank = it->addr_vec[m_rank_addr_idx];
            if (maint_rank != demand_rank) continue;

            if (fc == refab_id || fc == rfmab_id) {
                // Whole-rank scope.
                return true;
            }
            if (fc == rfmsb_id) {
                // Same-bank-index across all bank groups in the rank.
                if (it->addr_vec[m_bank_addr_idx] == demand_bank) return true;
                continue;
            }
            // fc == rfmpb_id: exact (rank, bg, bank).
            if (it->addr_vec[m_bankgroup_addr_idx] == demand_bg &&
                it->addr_vec[m_bank_addr_idx]      == demand_bank) {
                return true;
            }
        }
        return false;
    }

    void tick() override {
        m_clk++;

        // Queue-length running totals.
        s_queue_len          += m_read_buffer.size() + m_write_buffer.size() + m_priority_buffer.size() + pending.size();
        s_read_queue_len     += m_read_buffer.size() + pending.size();
        s_write_queue_len    += m_write_buffer.size();
        s_priority_queue_len += m_priority_buffer.size();

        serve_completed_reads();
        m_refresh->tick();
        m_scheduler->tick();

        // Schedule a request.
        ReqBuffer::iterator req_it;
        ReqBuffer* buffer = nullptr;
        bool request_found = schedule_request(req_it, buffer);

        m_rowpolicy->update(request_found, req_it);
        for (auto plugin : m_plugins) plugin->update(request_found, req_it);

        // Issue.
        if (request_found) {
            m_dram->issue_command(req_it->command, req_it->addr_vec);

            if (req_it->command == req_it->final_command) {
                if (req_it->type_id == Request::Type::Read) {
                    req_it->depart = m_clk + m_dram->m_read_latency;
                    pending.push_back(*req_it);
                    s_num_read_reqs++;
                } else if (req_it->type_id == Request::Type::Write) {
                    s_num_write_reqs++;
                } else {
                    s_num_other_reqs++;
                    if (req_it->command == m_dram->m_commands("REFab") ||
                        req_it->command == m_dram->m_commands("REFsb")) {
                        s_num_refresh_reqs++;
                    }
                    if (req_it->command == m_dram->m_commands("RFMab") ||
                        req_it->command == m_dram->m_commands("RFMsb") ||
                        req_it->command == m_dram->m_commands("RFMpb")) {
                        s_num_rfm_reqs++;
                    }
                }
                buffer->remove(req_it);
            } else {
                if (m_dram->m_command_meta(req_it->command).is_opening) {
                    if (m_active_buffer.enqueue(*req_it)) buffer->remove(req_it);
                }
            }
        }
    };

private:
    void serve_completed_reads() {
        if (pending.size()) {
            auto& req = pending[0];
            if (req.depart <= m_clk) {
                if (req.depart - req.arrive > 1) s_read_latency += req.depart - req.arrive;
                if (req.callback) req.callback(req);
                pending.pop_front();
            }
        };
    };

    void set_write_mode() {
        if (!m_is_write_mode) {
            if ((m_write_buffer.size() > m_wr_high_watermark * m_write_buffer.max_size) ||
                m_read_buffer.size() == 0) {
                m_is_write_mode = true;
            }
        } else {
            if ((m_write_buffer.size() < m_wr_low_watermark * m_write_buffer.max_size) &&
                m_read_buffer.size() != 0) {
                m_is_write_mode = false;
            }
        }
    };

    bool schedule_request(ReqBuffer::iterator& req_it, ReqBuffer*& req_buffer) {
        bool request_found = false;

        // 2.1  Active buffer first (avoid useless ACTs).
        if (req_it = m_scheduler->get_best_request(m_active_buffer); req_it != m_active_buffer.end()) {
            if (m_dram->check_ready(req_it->command, req_it->addr_vec)) {
                request_found = true;
                req_buffer    = &m_active_buffer;
            }
        }

        if (!request_found) {
            // 2.2.1  Priority buffer (maintenance: REFab/RFMab/RFMsb/RFMpb).
            if (m_priority_buffer.size() != 0) {
                auto& buffer = m_priority_buffer;
                req_it = m_scheduler->get_best_request(buffer);
                if (req_it != buffer.end()) {
                    if (m_dram->check_ready(req_it->command, req_it->addr_vec)) {
                        request_found = true;
                        req_buffer    = &m_priority_buffer;
                    }
                }
            }

            // 2.2.2  Demand requests (read / write).
            //        While ANY maintenance command (REFab/RFMab/RFMsb/RFMpb)
            //        on the affected bank-line is pending in the priority
            //        buffer, demand to that bank-line is deferred. 
            if (!request_found) {
                set_write_mode();
                auto& buffer = m_is_write_mode ? m_write_buffer : m_read_buffer;
                req_it = m_scheduler->get_best_request(buffer);
                if (req_it != buffer.end()) {
                    bool maint_clear = !is_demand_blocked_by_pending_maintenance(req_it->addr_vec);
                    bool ready       = m_dram->check_ready(req_it->command, req_it->addr_vec);

                    if (maint_clear && ready) {
                        request_found = true;
                        req_buffer    = &buffer;
                    } else if (!maint_clear && ready) {
                        s_num_demand_deferred_pending_maint++;
                        req_buffer = &buffer;
                    } else {
                        req_buffer = &buffer;
                    }
                }
            }
        }

        // Closing command must not conflict with anything still active.
        if (request_found && m_dram->m_command_meta(req_it->command).is_closing) {
            auto& rowgroup = req_it->addr_vec;
            for (auto _it = m_active_buffer.begin(); _it != m_active_buffer.end(); _it++) {
                auto& _it_rowgroup = _it->addr_vec;
                bool is_matching = true;
                for (int i = 0; i < m_bank_addr_idx + 1; i++) {
                    if (_it_rowgroup[i] != rowgroup[i] && _it_rowgroup[i] != -1 && rowgroup[i] != -1) {
                        is_matching = false;
                        break;
                    }
                }
                if (is_matching) { request_found = false; break; }
            }
        }

        // Row-policy stats for demand requests.
        if (request_found && req_buffer != &m_active_buffer) {
            if (req_it->type_id == Request::Type::Read ||
                req_it->type_id == Request::Type::Write) {
                auto& req_meta = m_dram->m_command_meta(req_it->command);
                int source_id  = req_it->source_id >= 0 ? req_it->source_id : 0;
                int increment  = req_it->source_id >= 0 ? 1 : 0;
                if (req_meta.is_accessing) { s_core_row_hits[source_id]      += increment; s_num_row_hits++; }
                if (req_meta.is_opening)   { s_core_row_misses[source_id]    += increment; s_num_row_misses++; }
                if (req_meta.is_closing)   { s_core_row_conflicts[source_id] += increment; s_num_row_conflicts++; }
            }
        }
        return request_found;
    }

    void finalize() override {
        s_read_latency_avg       = (double)s_read_latency       / (double)s_num_read_reqs;
        s_queue_len_avg          = (double)s_queue_len          / (double)m_clk;
        s_read_queue_len_avg     = (double)s_read_queue_len     / (double)m_clk;
        s_write_queue_len_avg    = (double)s_write_queue_len    / (double)m_clk;
        s_priority_queue_len_avg = (double)s_priority_queue_len / (double)m_clk;

        // 8K REFs per tREFW, summed across ranks.
        s_num_ref_windows = (double)s_num_refresh_reqs / (double)(8192 * m_dram->get_level_size("rank"));
    }
};
}   // namespace Ramulator
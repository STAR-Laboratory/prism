#ifndef RAMULATOR_PLUGIN_PRISM_H_
#define RAMULATOR_PLUGIN_PRISM_H_

#include "dram/dram.h"

#include <limits>
#include <unordered_map>

namespace Ramulator {

class IPRISM {
public:
    // ABO state machine,
    //   NORMAL        -- no recovery pending; controller serves demand freely
    //   PRE_RECOVERY  -- Alert asserted; wait tABO_ACT before any RFMab fires
    //   RECOVERY      -- RFMabs may issue; controller drains alert buffer
    //   DELAY         -- recovery done; wait ABO_Delay activations before
    //                    a new Alert may be asserted
    enum class ABOState {
        NORMAL,
        PRE_RECOVERY,
        RECOVERY,
        DELAY
    };

    virtual ~IPRISM() = default;

    // ---- Controller-facing queries (read-only) -----------------------------

    // Absolute cycle at which the next RECOVERY is allowed to start, i.e.
    // the first cycle RFMab may legally issue. Returns
    // std::numeric_limits<Clk_t>::max() when no recovery is pending.
    // Used by the controller to (a) arm PREall+RFMab setup ~tRP cycles ahead
    // and (b) reject demand/active requests that wouldn't fit before that
    // cycle (via min_cycles_with_preall).
    virtual Clk_t next_recovery_cycle() = 0;

    // Current ABO state. The controller blocks RFMab issue while
    // PRE_RECOVERY to enforce tABO_ACT (JEDEC: <=180 ns).
    virtual ABOState get_state() = 0;

    // Number of RFMabs the controller must issue per rank during the next
    // RECOVERY. Set by the plugin from its YAML knob (default 1) and may
    // vary across runs.
    virtual int get_num_abo_recovery_refs() = 0;

    // ---- Fits-before-recovery helper ---------------------------------------
    //
    // Returns a lower bound on the cycles between *issuing* the given request
    // and the earliest cycle a PREall to its rank could finish. The
    // controller uses this in its fits check:
    //
    //     if (m_clk + min_cycles_with_preall(req) < next_recovery_cycle())
    //         // safe to issue req before recovery starts
    //
    // For commands not in the table (e.g. internal maintenance), returns 0 —
    // those bypass the fits check.

    int min_cycles_with_preall(const ReqBuffer::iterator& req) {
        return min_cycles_with_preall(*req);
    }

    int min_cycles_with_preall(const Request& req) {
        auto it = cmd_to_min_cycles.find(req.command);
        return it == cmd_to_min_cycles.end() ? 0 : it->second;
    }

    // ---- One-time setup, called by the concrete plugin in its setup() ------
    //
    // Idempotent: safe to call more than once (long-running tests sometimes
    // re-initialize plugins, in which case the first call wins).
    void init_dram_params(IDRAM* dram) {
        if (dram_params_initialized) return;

        // Cycles from ACT to a precharge that fully retires the row.
        // - Read path:  ACT -> RD -> tRTP -> PRE -> tRP
        // - Write path: ACT -> WR -> nCWL+nBL+nWR -> PRE -> tRP
        const int nRP   = dram->m_timing_vals("nRP");
        const int nRAS  = dram->m_timing_vals("nRAS");
        const int nRTP  = dram->m_timing_vals("nRTP");
        const int nCWL  = dram->m_timing_vals("nCWL");
        const int nBL   = dram->m_timing_vals("nBL");
        const int nWR   = dram->m_timing_vals("nWR");

        const int write_to_pre = nCWL + nBL + nWR;
        // 
        read_cycles  = nRAS + nRTP + nRP;
        write_cycles = nRAS + write_to_pre + nRP;

        cmd_to_min_cycles[dram->m_commands("ACT")]   = dram->m_timing_vals("nRC");
        cmd_to_min_cycles[dram->m_commands("RD")]    = nRTP + nRP;
        cmd_to_min_cycles[dram->m_commands("WR")]    = write_to_pre + nRP;
        cmd_to_min_cycles[dram->m_commands("RFMsb")] = dram->m_timing_vals("nRFMsb");
        cmd_to_min_cycles[dram->m_commands("RFMab")] = dram->m_timing_vals("nRFM1");
        cmd_to_min_cycles[dram->m_commands("REFsb")] = dram->m_timing_vals("nRFCsb");
        cmd_to_min_cycles[dram->m_commands("REFab")] = dram->m_timing_vals("nRFC1");

        dram_params_initialized = true;
    }

protected:
    // Exposed to derived plugin classes so they can add custom entries
    // (e.g. RFMpb, VRR) if the workload requires.
    std::unordered_map<int, int> cmd_to_min_cycles;
    int read_cycles  = -1;
    int write_cycles = -1;

private:
    bool dram_params_initialized = false;

};      //  class IPRISM

}       //  namespace Ramulator

#endif // RAMULATOR_PLUGIN_PRISM_H_
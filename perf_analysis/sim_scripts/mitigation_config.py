"""
Shared configuration and mitigation logic for PrISM Ramulator2 simulations.

This module defines simulation constants, parameter-set generation helpers,
and the per-mitigation Ramulator2 configuration logic that is reused across
all figure-specific run_config_fig*.py files.
"""

import itertools
from calc_rh_parameters import *

# ----------------------------------------------------------------------
# Simulation control
# ----------------------------------------------------------------------
NUM_EXPECTED_INSTS = 250_000_000  # 8 cores: 250M per core => 2B total
NUM_MAX_CYCLES = 0                # 0 = no cycle cap

# ----------------------------------------------------------------------
# Ramulator2 controller / scheduler choices
# ----------------------------------------------------------------------
CONTROLLER = "BHDRAMController"
SCHEDULER  = "BHScheduler"

# ----------------------------------------------------------------------
# DRAM organization
# ----------------------------------------------------------------------
NUM_RANKS    = 1
NUM_CHANNELS = 1

# DDR5 interface speeds (MT/s) to sweep
interface_speeds = [8000]

# Field order used in stat-string generation
PARAM_STR_LIST = ["mitigation", "interface_speed", "NRH", "TREF", "pmq_capacity"]


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------
def is_prism_mitigation(mitigation):
    """Return True if `mitigation` is a PrISM variant."""
    return mitigation.startswith("PrISM")


def get_multicore_params_list(mitigation_list, interface_speeds,
                              NRH_lists, TREF_lists, PMQ_CAPACITY_LIST):
    """Cartesian product of all simulation parameters.

    PrISM variants are duplicated across PMQ_CAPACITY_LIST. Non-PrISM
    mechanisms receive pmq_capacity=None and are not duplicated.
    """
    params = []
    for mitigation, interface_speed, NRH, TREF in itertools.product(
        mitigation_list, interface_speeds, NRH_lists, TREF_lists
    ):
        if is_prism_mitigation(mitigation):
            for pmq_capacity in PMQ_CAPACITY_LIST:
                params.append((mitigation, interface_speed, NRH, TREF, pmq_capacity))
        else:
            params.append((mitigation, interface_speed, NRH, TREF, None))
    return params


def make_stat_str(param_list, delim="_"):
    """Build a filename-friendly stat string from a parameter tuple."""
    tokens = []
    for name, param in zip(PARAM_STR_LIST, param_list):
        if param is None:
            continue
        if name == "pmq_capacity":
            tokens.append(f"PMQ{param}")
        else:
            tokens.append(str(param))
    return delim.join(tokens)


def resolve_pmq_capacity(default_capacity, pmq_capacity):
    return default_capacity if pmq_capacity is None else pmq_capacity


# ----------------------------------------------------------------------
# Mitigation configuration
# ----------------------------------------------------------------------
def add_mitigation(config, mitigation, interface_speed, NRH, TREF, pmq_capacity=None):
    """Mutate `config` to apply the requested mitigation configuration."""
    # Common frontend / controller settings
    config['Frontend']['inst_window_depth']     = 512
    config['Frontend']['llc_num_mshr_per_core'] = 32
    config['Frontend']['llc_capacity_per_core'] = '2MB'
    config['MemorySystem'][CONTROLLER]['impl']  = 'OPTDRAMController'

    config['MemorySystem']['DRAM']['org']['rank']    = NUM_RANKS
    config['MemorySystem']['DRAM']['org']['channel'] = NUM_CHANNELS
    config['MemorySystem'][CONTROLLER]['RowPolicy']['impl'] = 'ClosedRowPolicy'
    config['MemorySystem'][CONTROLLER]['RowPolicy']['cap']  = 1

    _set_dram_timing(config, interface_speed)

    if mitigation in ("MINT"):
        BAT = get_mint_parameters(NRH)
        config['MemorySystem'][CONTROLLER]['plugins'].append({
            'ControllerPlugin': {
                'impl': 'RFMController',
                'bat': BAT,
                'rfm_type': 1,
                'targeted_ref_frequency': TREF,
                'max_rfm_postponement': 4,
                'num_early_counter_reset': True,
            }
        })

    elif mitigation == "QPRAC":
        NBO = get_qprac_parameters(NRH)
        _setup_prac_controller(config)
        config['MemorySystem'][CONTROLLER]['plugins'].append({
            'ControllerPlugin': {
                'impl': 'QPRAC',
                'abo_delay_acts': 1,
                'abo_recovery_refs': 1,
                'abo_act_ns': 180,
                'abo_threshold': NBO,
                'psq_size': 5,
                'targeted_ref_frequency': TREF,
                'proactive_mitigation_th': int(NBO / 2),
                'enable_opportunistic_mitigation': True,
            }
        })

    elif mitigation in ("PrISM", "PrISM-MOP", "PrISM-Rand"):
        _add_prism(config, mitigation, NRH, TREF, pmq_capacity)

    elif mitigation == "Baseline":
        pass  # no mitigation

    else:
        raise ValueError(f"Unknown mitigation: {mitigation!r}")


# ----------------------------------------------------------------------
# Internal helpers
# ----------------------------------------------------------------------
def _setup_prac_controller(config):
    config['MemorySystem']['DRAM']['PRAC'] = True
    config['MemorySystem'][CONTROLLER]['impl']               = 'PRACOPTController'
    config['MemorySystem'][CONTROLLER][SCHEDULER]['impl']    = 'PRACScheduler'


def _set_dram_timing(config, interface_speed):
    presets = {
        3200: ('DDR5_3200BN', 4, 10),
        4800: ('DDR5_4800B',  3,  5),
        6400: ('DDR5_6400B',  4,  5),
        8000: ('DDR5_8000B',  1,  1),
    }
    if interface_speed not in presets:
        raise ValueError(f"Unsupported interface speed: {interface_speed}")
    preset, memsys_ratio, fe_ratio = presets[interface_speed]
    config['MemorySystem']['DRAM']['timing']['preset'] = preset
    config['MemorySystem']['clock_ratio']               = memsys_ratio
    config['Frontend']['clock_ratio']                   = fe_ratio


def _add_prism(config, mitigation, NRH, TREF, pmq_capacity):
    R, L, act_rate = get_prism_parameters(NRH)
    config['MemorySystem']['DRAM']['PrISM']    = True
    config['MemorySystem'][CONTROLLER]['impl'] = 'PRISMDRAMController'

    # Address mapping:
    #   PrISM      : MOP for TRH-D >= 500, Rubix for ultra-low TRH-D (< 500).
    #   PrISM-Rand : always Rubix randomization.
    #   PrISM-MOP  : always MOP (i.e., do not randomize).
    if mitigation == "PrISM-Rand" or (mitigation == "PrISM" and NRH < 500):
        config['MemorySystem']['AddrMapper'] = {
            'impl': 'RubixPerm4CL',
            'rubix_gang_bits': 0,
            'rubix_perm_bits': 0,
        }

    config['MemorySystem'][CONTROLLER]['plugins'].append({
        'ControllerPlugin': {
            'impl': 'PRISM',
            'targeted_ref_frequency': TREF,
            'shq_length': L,
            'max_acts_per_mitigation': act_rate,
            'sampled_rows_per_mitigation': R,
            'mhq_length': 0,
            'pmq_capacity': resolve_pmq_capacity(8, pmq_capacity),
            'pmq_threshold': 4,
        }
    })

    # Add a proactive RFM only when the default rate or TRR cadence requires it
    if act_rate != 72 or TREF != 1:
        config['MemorySystem'][CONTROLLER]['plugins'].append({
            'ControllerPlugin': {
                'impl': 'RFMController',
                'bat': act_rate,
                'rfm_type': 1,
                'targeted_ref_frequency': TREF,
                'max_rfm_postponement': 4,
                'num_early_counter_reset': True,
            }
        })


if __name__ == "__main__":
    # Sanity check: print parameter set for default sweep
    test_params = get_multicore_params_list(
        ["Baseline", "PrISM"], [8000], [500], [2], [16]
    )
    for p in test_params:
        print(p)
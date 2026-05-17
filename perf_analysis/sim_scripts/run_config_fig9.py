"""Parameters for Figure 9 (TRR rate sensitivity at TRH-D = 500).

NOTE: This config assumes run_slurm_fig7_8.sh has been executed first.
Fig. 7/8 already covers Baseline and (NRH=500, TREF=2) for all mechanisms,
so we omit them here to avoid redundant simulations.

The unified results/ directory means the plotting script for Fig. 9 will
read TREF=2 data from the Fig. 7/8 runs and TREF in {0, 1, 4, 8} from here.
"""
from mitigation_config import *

mitigation_list   = ['MINT', 'QPRAC', 'PrISM']
NRH_lists         = [500]
TREF_lists        = [1, 4, 8, 0]   # 0 = no TRR; TREF=2 is provided by Fig. 7/8
PMQ_CAPACITY_LIST = [16]
"""Parameters for Table VI (PMQ size sensitivity at TRH-D = 500).

NOTE: This config assumes run_slurm_fig7_8.sh has been executed first.
Fig. 7/8 already covers PrISM at the default (NRH=500, TREF=2, PMQ=16),
so PMQ=16 is omitted here. The plotting script for Table VI will read
the PMQ=16 row from the Fig. 7/8 results in the unified results/ directory.

Only PrISM is varied: Baseline is also provided by Fig. 7/8.
"""
from mitigation_config import *

mitigation_list   = ['PrISM']
NRH_lists         = [500]
TREF_lists        = [2]
PMQ_CAPACITY_LIST = [4, 8, 32]   # PMQ=16 is provided by Fig. 7/8
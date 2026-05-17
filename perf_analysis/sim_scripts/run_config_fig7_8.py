"""Parameters for Figures 7 and 8 (main performance results vs MINT and QPRAC)."""
from mitigation_config import *

mitigation_list   = ['Baseline', 'MINT', 'QPRAC', 'PrISM']
NRH_lists         = [250, 500, 1000]
TREF_lists        = [2]   # default: one TRR per two tREFIs
PMQ_CAPACITY_LIST = [16]  # default PMQ size
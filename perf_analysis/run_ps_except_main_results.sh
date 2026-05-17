#!/bin/bash
# ----------------------------------------------------------------------
# PrISM -- Run non-main experiments on a personal server (Fig. 9 + Table VI)
#
# Assumes run_ps_fig7_8.sh has already been executed: Fig. 9 and Table VI
# reuse data points from the main run (see run_config_fig9.py and
# run_config_table6.py).
# ----------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
trap 'cd "$SCRIPT_DIR"' EXIT

echo "[INFO] Running Figure 9 experiments (TRR sensitivity)"
bash "$SCRIPT_DIR/run_ps_fig9.sh"

echo "[INFO] Running Table VI experiments (PMQ size sensitivity)"
bash "$SCRIPT_DIR/run_ps_table6.sh"

echo "[INFO] Non-main experiments complete."
#!/bin/bash
# ----------------------------------------------------------------------
# PrISM -- Run Figures 7 and 8 experiments on a personal server
#
# This script:
#   1. Generates Ramulator2 configs and direct-invocation commands
#   2. Runs them in parallel using PERSONAL_RUN_THREADS workers
#
# It is normally invoked by run_artifact.sh, but can be run standalone
# as long as run_prerequisite.sh has been executed first.
# ----------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
trap 'cd "$SCRIPT_DIR"' EXIT

# Number of concurrent ramulator2 processes (override via env var)
export PERSONAL_RUN_THREADS="${PERSONAL_RUN_THREADS:-40}"

cd "$SCRIPT_DIR/sim_scripts"

RAMULATOR_DIR="$PWD/.."
BASE_CONFIG="$PWD/../configs/DDR5_baseline_closed_mitigation.yaml"
TRACE_DIR="$PWD/../cputraces"
RESULT_DIR="$PWD/../results"

echo "[INFO] Generating Ramulator2 configurations and run commands (Fig. 7 and 8)"
echo "[INFO]   Mode             : personal"
echo "[INFO]   Parallel threads : $PERSONAL_RUN_THREADS"
echo "[INFO]   Result directory : $RESULT_DIR"

python3 setup_run.py \
    --mode personal \
    --run_config run_config_fig7_8 \
    --ramulator_directory "$RAMULATOR_DIR" \
    --working_directory "$PWD" \
    --base_config "$BASE_CONFIG" \
    --trace_directory "$TRACE_DIR" \
    --result_directory "$RESULT_DIR"

echo "[INFO] Running Ramulator2 simulations locally (this may take a long time)"
python3 execute_run_script.py

echo "[INFO] All simulations complete."

rm -f "$PWD/run.sh"
#!/bin/bash
# ----------------------------------------------------------------------
# PrISM -- Run Figures 7 and 8 experiments (main performance results)
#
# Figures 7 and 8 share the same simulation set; this script:
#   1. Generates Ramulator2 configs and SLURM job scripts
#   2. Submits the jobs to SLURM
#
# It is normally invoked by run_artifact.sh, but can be run standalone
# as long as run_prerequisite.sh has been executed first (i.e., traces
# downloaded and the ramulator2 binary built at the repo root).
# ----------------------------------------------------------------------
set -euo pipefail

# Resolve script directory so this works regardless of caller's CWD
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Always return to the repo root on exit (success, failure, or interrupt)
trap 'cd "$SCRIPT_DIR"' EXIT

# ----------------------------------------------------------------------
# SLURM configuration
# Values can be overridden via env vars exported by run_artifact.sh.
# When running standalone, the defaults below are used.
# ----------------------------------------------------------------------
SLURM_PART_NAME="${SLURM_PART_NAME:-skylake}"
SLURM_PART_DEF_MEM="${SLURM_PART_DEF_MEM:-6G}"
SLURM_PART_BIG_MEM="${SLURM_PART_BIG_MEM:-24G}"

# ----------------------------------------------------------------------
# Move into sim_scripts/ so the Python helpers run in their expected CWD
# (they create a run.sh in their working directory)
# ----------------------------------------------------------------------
cd "$SCRIPT_DIR/sim_scripts"

# Paths point back to the repo root (one level above sim_scripts/)
RAMULATOR_DIR="$PWD/.."
BASE_CONFIG="$PWD/../configs/DDR5_baseline_closed_mitigation.yaml"
TRACE_DIR="$PWD/../cputraces"
RESULT_DIR="$PWD/../results"

echo "[INFO] Generating Ramulator2 configurations and SLURM job scripts (Fig. 7 and 8)"
echo "[INFO]   Partition       : $SLURM_PART_NAME"
echo "[INFO]   Default memory  : $SLURM_PART_DEF_MEM"
echo "[INFO]   Large-job memory: $SLURM_PART_BIG_MEM"
echo "[INFO]   Result directory: $RESULT_DIR"

python3 setup_run.py \
    --mode slurm \
    --run_config run_config_fig7_8 \
    --ramulator_directory "$RAMULATOR_DIR" \
    --working_directory "$PWD" \
    --base_config "$BASE_CONFIG" \
    --trace_directory "$TRACE_DIR" \
    --result_directory "$RESULT_DIR" \
    --partition_names "$SLURM_PART_NAME" \
    --partition_default_memories "$SLURM_PART_DEF_MEM" \
    --partition_big_memories "$SLURM_PART_BIG_MEM"

echo "[INFO] Submitting Ramulator2 jobs to SLURM"
python3 execute_run_script.py --slurm

echo "[INFO] Jobs submitted."

# Clean up the generated launcher
rm -f "$PWD/run.sh"
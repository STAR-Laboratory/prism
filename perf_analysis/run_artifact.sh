#!/bin/bash
# ----------------------------------------------------------------------
# PrISM (ISCA 2026) -- artifact runner
#
# Drives the Ramulator2-based performance evaluation that reproduces:
#   * Figures 7 and 8  : main performance results (TRH-D = 250/500/1000)
#   * Figure 9         : sensitivity to Target Row Refresh (TRR) rate
#   * Table VI         : sensitivity to Pending Mitigation Queue (PMQ) size
#
# Run from this directory:
#   ./run_artifact.sh --method <slurm|personal> --artifact <main|all>
# ----------------------------------------------------------------------
set -euo pipefail

# Resolve script directory so this works regardless of caller's CWD
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ----------------------------------------------------------------------
# Usage
# ----------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $0 --method <slurm|personal> --artifact <all|main>

Options:
  --method <slurm|personal>    Specify the execution method
  --artifact <all|main>        Specify the experiments to run:
                                 main: Figures 7 and 8 (main performance results)
                                 all : Figures 7-9 + Table VI
EOF
    exit 1
}

# ----------------------------------------------------------------------
# Run-time configuration
# ----------------------------------------------------------------------
# Personal-server: maximum concurrent threads
PERSONAL_RUN_THREADS=40

# SLURM configuration (edit if you use SLURM).
# NOTE: SLURM_PART_NAME must match a partition available on your cluster.
SLURM_PART_NAME="skylake"
SLURM_PART_DEF_MEM="6G"
SLURM_PART_BIG_MEM="24G"
MAX_SLURM_JOBS=1000

# ----------------------------------------------------------------------
# Argument parsing
# ----------------------------------------------------------------------
METHOD=""
ARTIFACT=""

while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --method)
            METHOD="${2:-}"
            shift 2
            ;;
        --artifact)
            ARTIFACT="${2:-}"
            shift 2
            ;;
        -h|--help)
            usage
            ;;
        *)
            echo "[ERROR] Unknown option: $1"
            usage
            ;;
    esac
done

if [[ -z "$METHOD" ]]; then
    echo "[ERROR] --method is required"
    usage
fi
if [[ -z "$ARTIFACT" ]]; then
    echo "[ERROR] --artifact is required"
    usage
fi
if [[ "$METHOD" != "slurm" && "$METHOD" != "personal" ]]; then
    echo "[ERROR] Invalid method: $METHOD (must be 'slurm' or 'personal')"
    usage
fi
if [[ "$ARTIFACT" != "all" && "$ARTIFACT" != "main" ]]; then
    echo "[ERROR] Invalid artifact choice: $ARTIFACT (must be 'all' or 'main')"
    usage
fi

# ----------------------------------------------------------------------
# 1-3. Install dependencies, download traces, build Ramulator2
# ----------------------------------------------------------------------
echo "---------------------------"
echo "#####################"
echo "[INFO] Step 1: Preparing environment (dependencies, traces, build)"
echo "#####################"
bash ./run_prerequisite.sh

# ----------------------------------------------------------------------
# 4. Run Ramulator2 experiments
# ----------------------------------------------------------------------
echo "---------------------------"
echo "#####################"
echo "[INFO] Step 2: Running PrISM experiments (method=$METHOD, artifact=$ARTIFACT)"
echo "#####################"

# Export SLURM / personal config so child scripts can read them
export PERSONAL_RUN_THREADS
export MAX_SLURM_JOBS
export SLURM_PART_NAME
export SLURM_PART_DEF_MEM
export SLURM_PART_BIG_MEM

if [[ "$METHOD" == "slurm" ]]; then
    echo "[INFO] Running experiments with SLURM"
    echo "[INFO]   Partition       : $SLURM_PART_NAME"
    echo "[INFO]   Default memory  : $SLURM_PART_DEF_MEM"
    echo "[INFO]   Large-job memory: $SLURM_PART_BIG_MEM"
    echo "[INFO]   Max jobs        : $MAX_SLURM_JOBS"

    if [[ "$ARTIFACT" == "main" ]]; then
        echo "[INFO] Running main experiments (Figures 7 and 8)"
        bash ./run_slurm_fig7_8.sh
    else
        echo "[INFO] Running all experiments (Figures 7-9, Table VI)"
        echo "[INFO] Running experiments for Figures 7 and 8 (main performance)"
        bash ./run_slurm_fig7_8.sh
        echo "[INFO] Running experiments for Figure 9 (TRR sensitivity)"
        bash ./run_slurm_fig9.sh
        echo "[INFO] Running experiments for Table VI (PMQ size sensitivity)"
        bash ./run_slurm_table6.sh
    fi

else  # METHOD == "personal"
    echo "[INFO] Running experiments on a personal server"
    echo "[INFO]   Parallel threads: $PERSONAL_RUN_THREADS"

    if [[ "$ARTIFACT" == "main" ]]; then
        echo "[INFO] Running main experiments (Figures 7 and 8)"
        bash ./run_ps_fig7_8.sh
    else
        echo "[INFO] On a personal server with limited resources (e.g., < 256GB DRAM,"
        echo "       < 40 cores), running all experiments can take several days."
        echo "       Consider running --artifact main first to review the main results."
        echo "[INFO] Running all experiments (Figures 7-9, Table VI)"
        bash ./run_ps_fig7_8.sh
        bash ./run_ps_except_main_results.sh
    fi
fi

echo "---------------------------"
echo "[INFO] All requested experiments submitted/completed."
echo "[INFO] Once jobs finish, use ./plot_main_figures.sh or ./plot_all_figures.sh"
echo "       to regenerate the figures."
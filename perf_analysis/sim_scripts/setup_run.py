"""
Generate Ramulator2 configurations and a launcher (run.sh) for PrISM
experiments. Supports two execution modes:

  --mode slurm    : emit sbatch commands; run.sh submits jobs to SLURM.
  --mode personal : emit direct ramulator2 invocations; run.sh is consumed
                    by execute_run_script.py (without --slurm) and runs
                    locally in PERSONAL_RUN_THREADS workers.

Reads simulation parameters from a per-figure run_config module
(e.g., run_config_fig7_8), then emits per-job scripts and a top-level
run.sh in the working directory.

Optional environment variables:
  SLURM_EXTRA_ARGS   Extra arguments appended to every sbatch invocation
                     (slurm mode only), e.g.
                     "--account=myacct --mail-user=me@example.com --mail-type=FAIL"
"""

import os
import copy
import yaml
import argparse
import importlib

# ----------------------------------------------------------------------
# CLI
# ----------------------------------------------------------------------
argparser = argparse.ArgumentParser(
    prog="setup_run",
    description="Generate Ramulator2 launcher (slurm or personal mode) for PrISM.",
)
argparser.add_argument("-m",   "--mode", choices=["slurm", "personal"], default="slurm",
                       help="Execution mode: 'slurm' emits sbatch commands; "
                            "'personal' emits direct ramulator2 invocations.")
argparser.add_argument("-rc",  "--run_config",                  required=True,
                       help="Run-config module to import (e.g., run_config_fig7_8)")
argparser.add_argument("-rmd", "--ramulator_directory",         required=True)
argparser.add_argument("-wd",  "--working_directory",           required=True)
argparser.add_argument("-bc",  "--base_config",                 required=True)
argparser.add_argument("-td",  "--trace_directory",             required=True)
argparser.add_argument("-rd",  "--result_directory",            required=True)

# SLURM-only arguments (validated below in slurm mode).
argparser.add_argument("-pns", "--partition_names",             default="")
argparser.add_argument("-pdms","--partition_default_memories",  default="")
argparser.add_argument("-pbms","--partition_big_memories",      default="")
args = argparser.parse_args()

# ----------------------------------------------------------------------
# Import the figure-specific config dynamically
# ----------------------------------------------------------------------
cfg = importlib.import_module(args.run_config)

RAMULATOR_DIR    = args.ramulator_directory
WORK_DIR         = args.working_directory
BASE_CONFIG_FILE = args.base_config
TRACE_DIR        = args.trace_directory
RESULT_DIR       = args.result_directory
MODE             = args.mode

# ----------------------------------------------------------------------
# Mode-specific validation and setup
# ----------------------------------------------------------------------
PARTITION_CONFIGS = None
SBATCH_BASE = None

if MODE == "slurm":
    if not (args.partition_names and args.partition_default_memories
            and args.partition_big_memories):
        raise SystemExit(
            "[ERROR] --partition_names, --partition_default_memories, and "
            "--partition_big_memories are required in slurm mode."
        )
    PARTITION_NAMES    = [x.strip() for x in args.partition_names.split(",")]
    PARTITION_DEF_MEMS = [x.strip() for x in args.partition_default_memories.split(",")]
    PARTITION_BIG_MEMS = [x.strip() for x in args.partition_big_memories.split(",")]

    if not (len(PARTITION_NAMES) == len(PARTITION_DEF_MEMS) == len(PARTITION_BIG_MEMS)):
        raise SystemExit(
            "[ERROR] partition_names, partition_default_memories, and "
            "partition_big_memories must have the same number of "
            "comma-separated entries."
        )

    PARTITION_CONFIGS = list(zip(PARTITION_NAMES, PARTITION_DEF_MEMS, PARTITION_BIG_MEMS))

    # Optional user-specific sbatch arguments (account, email, etc.)
    SLURM_EXTRA_ARGS = os.environ.get("SLURM_EXTRA_ARGS", "")
    SBATCH_BASE = (
        f"sbatch --cpus-per-task=1 --nodes=1 --ntasks=1 --time=72:00:00 "
        f"{SLURM_EXTRA_ARGS}"
    ).strip()

# ----------------------------------------------------------------------
# Common setup
# ----------------------------------------------------------------------
CMD_HEADER = "#!/bin/bash"
CMD        = f"{RAMULATOR_DIR}/ramulator2"

with open(BASE_CONFIG_FILE, "r") as f:
    BASE_CONFIG = yaml.safe_load(f)
if BASE_CONFIG is None:
    raise SystemExit(f"[ERROR] Could not read base config: {BASE_CONFIG_FILE}")

BASE_CONFIG["Frontend"]["num_expected_insts"] = cfg.NUM_EXPECTED_INSTS
if cfg.NUM_MAX_CYCLES > 0:
    BASE_CONFIG["Frontend"]["num_max_cycles"] = cfg.NUM_MAX_CYCLES

# Pre-create result subdirectories per mitigation.
# SLURM mode writes a separate errors/ stream via --error=...; personal mode
# folds stderr into the stats file with `2>&1`, so no errors/ dir needed.
RESULT_SUBDIRS = ["stats", "configs", "cmd_count"]
if MODE == "slurm":
    RESULT_SUBDIRS.append("errors")

for mitigation in cfg.mitigation_list:
    for sub in RESULT_SUBDIRS:
        os.makedirs(f"{RESULT_DIR}/{mitigation}/{sub}", exist_ok=True)

# ----------------------------------------------------------------------
# Workload set (8-core homogeneous)
# ----------------------------------------------------------------------
traces = [
    "401.bzip2", "403.gcc", "429.mcf", "433.milc", "434.zeusmp", "435.gromacs",
    "436.cactusADM", "437.leslie3d", "444.namd", "445.gobmk", "447.dealII",
    "450.soplex", "456.hmmer", "458.sjeng", "459.GemsFDTD", "462.libquantum",
    "464.h264ref", "470.lbm", "471.omnetpp", "473.astar", "481.wrf",
    "482.sphinx3", "483.xalancbmk", "500.perlbench", "502.gcc", "505.mcf",
    "507.cactuBSSN", "508.namd", "510.parest", "511.povray", "519.lbm",
    "520.omnetpp", "523.xalancbmk", "525.x264", "526.blender", "531.deepsjeng",
    "538.imagick", "541.leela", "544.nab", "549.fotonik3d", "557.xz",
    "grep_map0", "h264_encode", "jp2_decode", "jp2_encode",
    "tpcc64", "tpch17", "tpch2", "tpch6",
    "wc_8443", "wc_map0",
    "ycsb_abgsave", "ycsb_aserver", "ycsb_bserver",
    "ycsb_cserver", "ycsb_dserver", "ycsb_eserver",
]

# 429.mcf is extremely memory-intensive. With 8 cores and the full instruction
# budget, a single simulation can exceed 2 days. We reduce its instruction count
# to 1/4 for runtime feasibility. Weighted speedup over this representative
# slice is consistent with the trends reported in the paper.
HEAVY_TRACE_REDUCTION = {"429.mcf": 4}

# Traces whose 8-core runs need the larger SLURM memory request
HIGH_MEMORY_TRACES = {
    "wc_8443", "wc_map0", "429.mcf", "459.GemsFDTD", "470.lbm", "505.mcf",
    "507.cactuBSSN", "519.lbm", "549.fotonik3d", "grep_map0",
}

NUM_CORES = 8


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------
def _get_partition_config(job_idx):
    return PARTITION_CONFIGS[job_idx % len(PARTITION_CONFIGS)]


def _build_slurm_cmd(job_idx, mitigation, tag, trace,
                     sbatch_filename, result_filename, error_filename):
    partition_name, partition_def_mem, partition_big_mem = _get_partition_config(job_idx)
    mem = partition_big_mem if trace in HIGH_MEMORY_TRACES else partition_def_mem
    return (
        f"{SBATCH_BASE} --chdir={WORK_DIR} --output={result_filename} "
        f"--error={error_filename} --mem={mem} --partition={partition_name} "
        f"--job-name='{tag}' {sbatch_filename}"
    )


def _build_personal_cmd(config_filename, result_filename):
    return f"{CMD} -f {config_filename} > {result_filename} 2>&1"


# ----------------------------------------------------------------------
# Run-command generation
# ----------------------------------------------------------------------
def get_multicore_run_commands():
    run_commands = []
    multicore_params = cfg.get_multicore_params_list(
        cfg.mitigation_list,
        cfg.interface_speeds,
        cfg.NRH_lists,
        cfg.TREF_lists,
        cfg.PMQ_CAPACITY_LIST,
    )

    # Baseline doesn't depend on NRH/TREF; run it once per interface_speed.
    # Use the first values in the per-figure lists as the canonical run.
    baseline_nrh  = cfg.NRH_lists[0]
    baseline_tref = cfg.TREF_lists[0]

    for param_tuple in multicore_params:
        mitigation, interface_speed, NRH, TREF, pmq_capacity = param_tuple

        if mitigation == "Baseline" and not (NRH == baseline_nrh and TREF == baseline_tref):
            continue

        stat_str = cfg.make_stat_str(param_tuple)

        for trace in traces:
            if mitigation == "Baseline":
                tag = f"{interface_speed}_{trace}"
            else:
                tag = f"{stat_str}_{trace}"

            result_filename    = f"{RESULT_DIR}/{mitigation}/stats/{tag}.txt"
            config_filename    = f"{RESULT_DIR}/{mitigation}/configs/{tag}.yaml"
            cmd_count_filename = f"{RESULT_DIR}/{mitigation}/cmd_count/{tag}.cmd.count"
            error_filename     = f"{RESULT_DIR}/{mitigation}/errors/{tag}.txt"  # slurm only

            run_config = copy.deepcopy(BASE_CONFIG)

            if trace in HEAVY_TRACE_REDUCTION:
                run_config["Frontend"]["num_expected_insts"] = (
                    cfg.NUM_EXPECTED_INSTS // HEAVY_TRACE_REDUCTION[trace]
                )

            run_config["MemorySystem"][cfg.CONTROLLER]["plugins"][0] \
                      ["ControllerPlugin"]["path"] = cmd_count_filename

            workload_paths = [f"{TRACE_DIR}/{trace}"] * NUM_CORES
            run_config["Frontend"]["traces"] = workload_paths

            cfg.add_mitigation(run_config, mitigation, interface_speed,
                               NRH, TREF, pmq_capacity)

            with open(config_filename, "w") as out:
                yaml.dump(run_config, out, default_flow_style=False)

            # SLURM mode also needs a per-job sbatch wrapper script
            sbatch_filename = f"{WORK_DIR}/run_scripts/{mitigation}_{tag}.sh"
            if MODE == "slurm":
                with open(sbatch_filename, "w") as sb:
                    sb.write(f"{CMD_HEADER}\n{CMD} -f {config_filename}\n")

            job_idx = len(run_commands)
            if MODE == "slurm":
                cmd = _build_slurm_cmd(job_idx, mitigation, tag, trace,
                                       sbatch_filename, result_filename, error_filename)
            else:
                cmd = _build_personal_cmd(config_filename, result_filename)

            run_commands.append(cmd)

    return run_commands


# ----------------------------------------------------------------------
# Emit run scripts and run.sh
# ----------------------------------------------------------------------
# SLURM mode emits per-job scripts; personal mode does not need them.
if MODE == "slurm":
    os.system(f"rm -rf {WORK_DIR}/run_scripts")
    os.makedirs(f"{WORK_DIR}/run_scripts", exist_ok=True)

commands = get_multicore_run_commands()

with open("run.sh", "w") as f:
    f.write(f"{CMD_HEADER}\n")
    for cmd in commands:
        f.write(f"{cmd}\n")
os.system("chmod uog+x run.sh")

print(f"[INFO] Mode             : {MODE}")
print(f"[INFO] Generated {len(commands)} command(s) in {os.path.abspath('run.sh')}")
if MODE == "slurm":
    print(f"[INFO] Per-job sbatch scripts in {WORK_DIR}/run_scripts/")
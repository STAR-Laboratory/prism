# PrISM Performance Analysis

This directory contains the Ramulator2 simulation infrastructure used to
reproduce the performance figures and table of the Loaded Dice (PrISM) paper
(ISCA 2026):

- **Figure 7** — Normalized weighted speedup vs. PRAC and MINT at
  TRH-D ∈ {250, 500, 1000}.
- **Figure 8** — RFMs per tREFI per channel, same configurations.
- **Figure 9** — Sensitivity to the Target Row Refresh (TRR) rate at TRH-D = 500.
- **Table VI** — Sensitivity to the PMQ size at TRH-D = 500.

---

## Directory layout

```
perf_analysis/
├── run_artifact.sh                  # Top-level driver (prerequisites + experiments)
├── run_prerequisite.sh              # Dependencies + traces + build (invoked by run_artifact.sh)
├── download_traces.sh               # Fetches CPU traces from Zenodo
├── build.sh                         # Builds Ramulator2
├── python_dependencies.txt
├── run_slurm_fig7_8.sh              # Per-figure SLURM runners
├── run_slurm_fig9.sh
├── run_slurm_table6.sh
├── run_ps_fig7_8.sh                 # Per-figure personal-server runners
├── run_ps_fig9.sh
├── run_ps_table6.sh
├── run_ps_except_main_results.sh
├── sim_scripts/
│   ├── mitigation_config.py         # Per-mitigation Ramulator2 config logic
│   ├── run_config_fig7_8.py         # Sweep params for Fig. 7/8
│   ├── run_config_fig9.py           # Sweep params for Fig. 9
│   ├── run_config_table6.py         # Sweep params for Table VI
│   ├── setup_run.py                 # Builds sbatch / direct-run scripts
│   ├── execute_run_script.py        # Submits jobs (slurm or local)
│   └── calc_rh_parameters.py        # RowHammer parameter helpers
├── plot_scripts/
│   ├── collate.py                   # Parses results -> canonical CSVs
│   ├── plot_fig7_8.py
│   ├── plot_fig9.py
│   └── gen_table6.py
├── configs/
│   └── DDR5_baseline_closed_mitigation.yaml
├── cputraces/                       # Populated by run_prerequisite.sh
├── results/                         # Ramulator2 outputs (per-mitigation)
│   └── collated/                    # Canonical CSVs from collate.py
├── plots/                           # Generated PDFs
├── tables/                          # Generated text tables
├── sample_results/                  # Committed reference outputs (CSVs + PDFs)
└── ramulator2                       # Binary, built by build.sh
```

---

## Quick start

### 1. Run the experiments

From this directory:

```bash
# On a SLURM cluster
./run_artifact.sh --method slurm    --artifact main   # Fig. 7 and 8 only
./run_artifact.sh --method slurm    --artifact all    # Fig. 7-9 + Table VI

# On a single machine (parallel via thread pool)
./run_artifact.sh --method personal --artifact main
./run_artifact.sh --method personal --artifact all
```

`run_artifact.sh` automatically invokes `run_prerequisite.sh` on the first
run, which installs Python dependencies, downloads the trace bundle from
Zenodo (~7.1GB), and builds Ramulator2. Subsequent runs reuse the existing
build and traces.

We recommend starting with `--artifact main` first to confirm the pipeline
works end-to-end before kicking off the full sweep.

### 2. Collate and plot

After all jobs finish:

```bash
cd plot_scripts
python3 collate.py
python3 plot_fig7_8.py
python3 plot_fig9.py
python3 gen_table6.py
```

The plot scripts write PDFs to `../plots/` and the table script writes a
plain-text file to `../tables/`.

---

## SLURM configuration

The SLURM runners pick up partition and memory defaults from environment
variables (or the defaults in `run_artifact.sh`). To match your cluster,
either edit `run_artifact.sh` or export the variables before invoking it:

```bash
export SLURM_PART_NAME=skylake
export SLURM_PART_DEF_MEM=6G
export SLURM_PART_BIG_MEM=24G
```

User-specific sbatch arguments (account, email, partition limits) are
supplied via `SLURM_EXTRA_ARGS`:

```bash
export SLURM_EXTRA_ARGS="--account=myacct --mail-user=me@example.com --mail-type=FAIL"
./run_artifact.sh --method slurm --artifact main
```

`SLURM_EXTRA_ARGS` is appended to every sbatch invocation. Leave it unset
if your cluster doesn't require account/notification flags.

---

## Reproducing individual figures

`run_artifact.sh` chains the per-figure runners together, but you can also
invoke each one directly:

| Figure / Table | SLURM runner            | Personal runner       |
|----------------|-------------------------|-----------------------|
| Fig. 7 and 8   | `run_slurm_fig7_8.sh`   | `run_ps_fig7_8.sh`    |
| Fig. 9         | `run_slurm_fig9.sh`     | `run_ps_fig9.sh`      |
| Table VI       | `run_slurm_table6.sh`   | `run_ps_table6.sh`    |

Fig. 9 and Table VI assume Fig. 7/8 has already run (they reuse common
data points). If you only want Fig. 9 or Table VI standalone, edit
`sim_scripts/run_config_figN.py` to include the points that would
otherwise come from Fig. 7/8.

---

## What the collated CSVs contain

`plot_scripts/collate.py` parses every `results/<mitigation>/stats/*.txt`
file and writes four canonical CSVs under `results/collated/`:

| CSV                    | Description |
|------------------------|-------------|
| `baseline_mpki.csv`    | Per-workload LLC-MPKI and row-buffer MPKI from Baseline. |
| `perf_normalized.csv`  | Per-workload weighted speedup normalized to Baseline, indexed by (workload, TRH-D, TREF_Freq), with MINT/QPRAC/PrISM side-by-side. PrISM uses the default PMQ size (16). Includes per-suite GMEAN rows and an `All (57)` summary row. Source for Figs. 7, 9. |
| `rfm_per_trefi.csv`    | Per-workload average RFMs per tREFI, same shape as `perf_normalized.csv`. Per-suite summaries use arithmetic mean. Source for Fig. 8. |
| `pmq_sweep.csv`        | PrISM normalized speedup across PMQ sizes {4, 8, 16, 32} at TRH-D=500, TREF=2. One row per workload with columns `PMQ4`, `PMQ8`, `PMQ16`, `PMQ32`. Source for Table VI. |

The plot scripts each read from the CSV they need (no manual filtering
required).

By default, `collate.py` clips normalized speedup at 1.0 to match the
paper figures. Pass `--no-clip-at-one` if you want raw normalized values.

---

## Sample results

Reference outputs from our run are committed under `sample_results/` to
help verify reproductions. The directory mirrors the layout that the
pipeline generates: `collated/` for CSVs, `plots/` for PDFs, and
`tables/` for plain-text tables. Specifically:

- `collated/baseline_mpki.csv`, `perf_normalized.csv`,
  `rfm_per_trefi.csv`, `pmq_sweep.csv` — reference outputs of
  `collate.py`.
- `plots/fig7_perf.pdf`, `fig8_rfm.pdf`, `fig9_trr_sensitivity.pdf` —
  reference PDFs matching the paper.
- `tables/table6_pmq_sensitivity.txt` — reference table.

After running the pipeline, you can diff your generated files against
these to confirm your reproduction matches:

```bash
diff -r sample_results/collated/ results/collated/
```

The PDFs are also useful as a visual sanity check before kicking off the
full sweep.

---

## Customization

### Different workload count or core count

Edit `sim_scripts/mitigation_config.py`:

```python
NUM_EXPECTED_INSTS = 250_000_000   # per core
```

Edit `sim_scripts/setup_run.py`:

```python
NUM_CORES = 8
```

### Different traces

Add or remove traces by editing the `traces` list in `sim_scripts/setup_run.py`
and the corresponding `BENCHMARK_SUITES` dict in `plot_scripts/collate.py`
(so per-suite GMEANs cover the right set).

### Different mitigations

`sim_scripts/mitigation_config.py` defines how each mitigation is configured
in the Ramulator2 YAML. Add a new branch in `add_mitigation()` to introduce
your own; then add the mitigation to the relevant `run_config_*.py` file.

---

## Software requirements

- GCC 12+ (tested with 12 and 15)
- CMake 3.20+
- Python 3.10+
- pip packages from `python_dependencies.txt`:
  matplotlib, pandas, seaborn, pyyaml, scipy, numpy
- (Optional) SLURM 20+ for cluster execution
- ZLIB development headers (apt: `zlib1g-dev` / brew: `zlib`)

---

## Hardware / runtime expectations

On SLURM with ~500-1000 concurrent jobs, both `--artifact main` and
`--artifact all` complete in roughly the same wall-clock:

| Workload class              | Typical per-job time |
|-----------------------------|----------------------|
| Most workloads              | ~1 hour              |
| High-memory-intensity runs  | ~4 hours             |
| `429.mcf` (longest)         | ~12 hours            |

Personal-server runs scale with `PERSONAL_RUN_THREADS` (default 40) and
take noticeably longer end-to-end since fewer jobs run in parallel.

Total disk usage is approximately 10GB.

---

## Trace bundle

The CPU traces downloaded by `download_traces.sh` are hosted on Zenodo and
were originally prepared for our prior **QPRAC** project (HPCA 2025) [1].
The bundle includes 57 workloads drawn from SPEC2006/2017, TPC, Hadoop,
MediaBench, and YCSB.

[1] J. Woo, S. Lin, P. J. Nair, A. Jaleel, and G. Saileshwar, *"QPRAC:
Towards Secure and Practical PRAC-based Rowhammer Mitigation using
Priority Queues,"* in *31st IEEE International Symposium on High-
Performance Computer Architecture (HPCA)*, 2025.

---

## Citation

If you use this artifact, please cite the PrISM paper:

```bibtex
@inproceedings{prism_isca2026,
  title     = {Loaded Dice: Solving the Non-Selection Problem for Scalable Probabilistic RowHammer Defense},
  author    = {Jeonghyun Woo and Junsu Kim and Aamer Jaleel and Prashant J. Nair},
  booktitle = {53rd Annual International Symposium on Computer Architecture (ISCA)},
  year      = {2026},
  series    = {ISCA'26},
}
```
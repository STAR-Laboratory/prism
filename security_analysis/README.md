# PrISM Security Analysis

This directory contains the analytical security model and Monte-Carlo
validation used to reproduce the security figures of the Loaded Dice (PrISM)
paper (ISCA 2026):

- **Figure 5** — Double-sided RowHammer threshold (TRH-D) vs. lookback
  window L at fixed W = 72, across R ∈ {2, ..., 9}, with Monte-Carlo
  validation overlaid on the analytical curves.
- **Figure 6** — Pareto frontier of TRH-D vs. SHQ entries across
  W ∈ {24, 36, 48, 60, 72}.

The analysis sweeps the attacker's row-count parameter A over the
single-copy regime (A ≥ W) and reports the worst-case TRH-D for each
configuration. See Section IV of the paper for the underlying recurrence.

---

## Directory layout

```
security_analysis/
├── prism_circular_security_analysis.py   # Analytical model + MC validation
├── plot_scripts/
│   ├── plot_figure5.py                   # TRH-D vs L at W=72
│   └── plot_figure6.py                   # TRH-D vs SHQ entries, sweep over W
├── results/                              # Generated CSVs
├── plots/                                # Generated PDFs
└── sample_results/                       # Committed reference outputs (CSVs + PDFs)
```

---

## Quick start

Install dependencies (only needed once):

```bash
pip3 install numpy pandas matplotlib seaborn
```

From this directory:

```bash
# Figure 5: fixed-W TRH-D vs L with Monte-Carlo validation
python3 prism_circular_security_analysis.py figure8 --w 72 \
    --output-dir ./results --mc-windows 1000000 --mc-seeds 5
python3 plot_scripts/plot_figure5.py

# Figure 6: joint W/R/L sweep
python3 prism_circular_security_analysis.py sweep-w \
    --w-values 24,36,48,60,72 --r-min 2 --r-max 18 --l-min 2 --l-max 200 \
    --output-dir ./results
python3 plot_scripts/plot_figure6.py
```

CSVs land in `results/` and PDFs in `plots/`.

---

## Subcommands

`prism_circular_security_analysis.py` has two subcommands:

| Subcommand | Purpose                                         | Consumed by         |
|------------|-------------------------------------------------|---------------------|
| `figure8`  | Fixed-W TRH-D vs L curves with MC validation    | `plot_figure5.py`   |
| `sweep-w`  | Joint (W, R, L) sweep                           | `plot_figure6.py`   |

Output filenames embed the safety-margin tag (e.g.,
`figure8_generalized_with_mc_W72_QTH4_ABO12.csv`) so different PMQ
configurations can coexist in the same directory.

---

## Configuration notes

### Safety margin (QTH and ABO_ACT)

Both subcommands accept `--qth` and `--abo-act`. The defaults reproduce
the paper's PMQ-16 configuration (QTH=4, ABO_ACT=12, total margin 16
single-row activations). To evaluate a different PMQ size, pass both
values explicitly. Plot scripts accept the same flags and locate CSVs
by filename tag.

### Monte-Carlo iteration count

The `figure8` subcommand defaults to `--mc-windows 1000000 --mc-seeds 5`
(5M samples per point), which already tracks the analytical curve
closely. We recommend the default for quick reproduction.

To reproduce the exact Monte-Carlo markers in the paper's Figure 5,
use `--mc-windows 5000000 --mc-seeds 5` (25M samples per point). This
produces a marginally smoother curve at higher cost (roughly 1 hour on
80 cores vs. ~10 minutes at the default).

Note that `find_min_threshold` is integer-valued, so 1-2 unit ±1 jitter
at the smallest TRH-D values is quantization, not noise, and will not
shrink further with additional MC iterations.

### ABO-safety constraint (W >= 4R)

Configurations must satisfy `W >= 4R` so the SHQ stays bounded under
chained ABO (see Section III-C of the paper). The sweep utilities
silently filter out configurations that violate this constraint.

---

## Sample results

Reference outputs from our run are committed under `sample_results/` to
help verify reproductions:

```
sample_results/
├── results/                 # Reference CSVs
│   ├── figure8_generalized_with_mc_W72_QTH4_ABO12.csv
│   └── sweep_W_R_L_QTH4_ABO12.csv
└── plots/                   # Reference PDFs matching the paper
    ├── sens_R_L_TRHD_W72_QTH4_ABO12.pdf
    └── sens_W_SHQ_TRHDbase_QTH4_ABO12.pdf
```

After running the pipeline, you can diff your generated files against
these to confirm your reproduction matches:

```bash
diff -r sample_results/results/ results/
```

The PDFs are also useful as a visual sanity check.

---

## Software requirements

- Python 3.10+
- numpy, pandas, matplotlib, seaborn

---

## Runtime expectations

Both subcommands run on a single machine and parallelize across
`os.cpu_count()` workers.

| Subcommand                                  | 32 cores       | 80 cores       |
|---------------------------------------------|----------------|----------------|
| `figure8` (default, `--mc-windows 1000000`) | ~30 minutes    | ~10 minutes    |
| `figure8 --mc-windows 5000000`              | several hours  | ~1 hour        |
| `sweep-w` (W/R/L grid, no MC)               | ~15-30 minutes | ~5-10 minutes  |

Total disk usage is well under 100MB.

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
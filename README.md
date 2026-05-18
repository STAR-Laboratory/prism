# Loaded Dice: Solving the Non-Selection Problem for Scalable Probabilistic RowHammer Defense

**To appear in the 53rd International Symposium on Computer Architecture (ISCA), 2026.**

[Jeonghyun Woo](https://jeonghyunwoo0306.github.io/)<sup>1*</sup>,
[Junsu Kim](https://junsu-kim0807.github.io/)<sup>1*</sup>,
[Aamer Jaleel](https://www.jaleels.org/ajaleel/)<sup>2</sup>,
[Prashant J. Nair](https://prashantnair.bitbucket.io/)<sup>1</sup>

<sup>1</sup> University of British Columbia &nbsp;&nbsp;
<sup>2</sup> NVIDIA &nbsp;&nbsp;
<sup>*</sup> Equal contribution

[Paper (PDF)](https://jeonghyunwoo0306.github.io/assets/pdf/ISCA26_Loaded_Dice.pdf) | [arXiv](#)

---

## Abstract

DRAM scaling has exacerbated the RowHammer vulnerability. To counter
this, JEDEC recently introduced Per Row Activation Counting (PRAC) with
the Alert Back-Off protocol as an optional DDR5 feature. While
promising, PRAC requires per-row counter cells that incur area
overhead, and updating them on every activation lengthens DRAM timing
parameters, degrading performance. Probabilistic mitigations such as
MINT offer a lower-cost alternative by randomly selecting and
mitigating rows within periodic mitigation windows. MINT is effective
at higher thresholds (≥ 1000), but at lower thresholds, it must raise
its mitigation rate to overcome the non-selection problem, where
heavily hammered rows can repeatedly escape sampling. This fixed-rate
scaling reduces effective memory bandwidth even when no attack is
present.

To overcome this limitation, we propose **PrISM**, an
intersection-based probabilistic mitigation that correlates sampled
rows across windows using a Sampled History Queue (SHQ). PrISM samples
a few activation slots per window, stores sampled-but-unmitigated rows
in the SHQ, and requests an additional mitigation through the existing
Alert Back-Off protocol when a sampled row reappears in this history.
This allows PrISM to increase mitigation only when persistent row
activity is observed, without globally increasing the fixed mitigation
rate. At the threshold of 500, PrISM incurs a negligible 0.2% average
slowdown compared to 14% for PRAC, with no DRAM array changes or
per-row counters and only 625B of SRAM per bank — one to two orders of
magnitude less than prior secure counter-based in-DRAM defenses.
Compared to MINT, PrISM provides better scalability at low thresholds,
reducing average slowdown from 10.7% to 1.5% at a threshold of 250, a
7.1× reduction.

---

## Repository layout

```
prism/
├── perf_analysis/         # Ramulator2 simulations (Figures 7, 8, 9; Table VI)
├── security_analysis/     # Analytical model + MC validation (Figures 5, 6)
├── LICENSE
└── README.md              # This file
```

Each subdirectory has its own README with detailed reproduction
instructions:

- [`perf_analysis/`](perf_analysis/README.md) — Ramulator2 infrastructure
  for the performance results. Supports both SLURM and single-machine
  execution.
- [`security_analysis/`](security_analysis/README.md) — Pure-Python
  analytical model and Monte-Carlo validation for the security claims.

---

## Quick start

Clone the repository and pick a sub-directory:

```bash
git clone https://github.com/STAR-Laboratory/prism.git
cd prism
```

To reproduce the **performance** results (Figures 7, 8, 9; Table VI):

```bash
cd perf_analysis
./run_artifact.sh --method slurm --artifact all   # or --method personal
```

To reproduce the **security** analysis (Figures 5, 6):

```bash
cd security_analysis
python3 prism_circular_security_analysis.py figure8 --w 72 \
    --output-dir ./results --mc-windows 1000000 --mc-seeds 5
python3 plot_scripts/plot_figure5.py
```

See the sub-directory READMEs for full details.

---

## Citation

```bibtex
@inproceedings{prism_isca2026,
  title     = {Loaded Dice: Solving the Non-Selection Problem for Scalable Probabilistic RowHammer Defense},
  author    = {Jeonghyun Woo and Junsu Kim and Aamer Jaleel and Prashant J. Nair},
  booktitle = {53rd Annual International Symposium on Computer Architecture (ISCA)},
  year      = {2026},
  series    = {ISCA'26},
}
```

---

## License

This repository is released under the [MIT License](LICENSE).

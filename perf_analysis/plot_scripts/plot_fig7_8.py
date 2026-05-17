"""
Render Figures 7 and 8 of the PrISM paper.

  Figure 7 -- Normalized weighted speedup vs. PRAC and MINT at TRH-D in
              {250, 500, 1000}, for high-memory-intensity workloads and
              per-suite geomeans.
  Figure 8 -- RFMs per tREFI per channel, same configurations and layout.

Both figures use:
  - Per-workload weighted-speedup from perf_normalized.csv (Fig. 7)
    or RFM frequency from rfm_per_trefi.csv (Fig. 8).
  - TREF_Freq = 2 (default TRR rate).
  - QPRAC bars are shared across all three TRH-D values (PRAC's overhead
    is TRH-D-independent in the evaluated regime).

The input CSVs are the condensed (workload, TRH-D, TREF_Freq) format from
collate.py, with MINT/QPRAC/PrISM as side-by-side columns. PrISM is at the
default PMQ size (16) already baked into the CSV.

Inputs (default ../results/collated/):
  perf_normalized.csv
  rfm_per_trefi.csv

Outputs (default ../plots/):
  fig7_perf.pdf
  fig8_rfm.pdf
"""

import argparse
from pathlib import Path

import matplotlib.patches as patches
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns
from matplotlib.patches import FancyArrowPatch
from matplotlib.transforms import blended_transform_factory


# =============================================================================
# Defaults / Constants
# =============================================================================
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_RESULTS_DIR = SCRIPT_DIR.parent / "results" / "collated"
DEFAULT_OUTPUT_DIR  = SCRIPT_DIR.parent / "plots"

# Filter applied to the unified CSVs.
DEFAULT_TREF = 2

# X-axis ordering: high-RBMPKI workloads, empty separator, then per-suite GMEANs.
WORKLOAD_ORDER = [
    "429.mcf", "470.lbm", "434.zeusmp", "519.lbm", "549.fotonik3d",
    "459.GemsFDTD", "450.soplex", "462.libquantum", "433.milc",
    "437.leslie3d", "510.parest", "520.omnetpp", "483.xalancbmk",
    "wc_8443", "wc_map0", "482.sphinx3", "tpch2",
    "",  # separator
    "SPEC2K6 (23)", "SPEC2K17 (18)", "TPC (4)",
    "Hadoop (3)", "MediaBench (3)", "YCSB (6)", "All (57)",
]

GEOMEAN_LABELS = {
    "SPEC2K6 (23)", "SPEC2K17 (18)", "TPC (4)",
    "Hadoop (3)", "MediaBench (3)", "YCSB (6)", "All (57)",
}

# (mitigation_column, TRH-D, plot_label)
# QPRAC's overhead is TRH-D-independent in the evaluated regime, so its single
# bar represents all three TRH-D values.
SERIES_SPECS = [
    ("QPRAC", 250,  r"PRAC:$T_{RH-D}=250/500/1000$"),
    ("MINT",  250,  r"MINT+RFM11:$T_{RH-D}=250$"),
    ("MINT",  500,  r"MINT+RFM24:$T_{RH-D}=500$"),
    ("MINT",  1000, r"MINT+RFM48:$T_{RH-D}=1000$"),
    ("PrISM", 250,  r"PrISM:$T_{RH-D}=250$"),
    ("PrISM", 500,  r"PrISM:$T_{RH-D}=500$"),
    ("PrISM", 1000, r"PrISM:$T_{RH-D}=1000$"),
]


# =============================================================================
# Data prep
# =============================================================================
def build_plot_df(csv_path: Path, value_col_name: str,
                  tref: int) -> tuple[pd.DataFrame, list[str]]:
    """Read a unified CSV (perf_normalized or rfm_per_trefi) and stack rows
    into the long format expected by seaborn.

    Returns (DataFrame with columns: workload, Methods, <value_col_name>,
    list of method labels in canonical order).
    """
    if not csv_path.exists():
        raise FileNotFoundError(
            f"Could not find {csv_path}. Run collate.py first.")
    df = pd.read_csv(csv_path)

    required = {"workload", "TRH-D", "TREF_Freq"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing required columns in {csv_path}: {missing}")

    rows = []
    for col, trh_d, label in SERIES_SPECS:
        if col not in df.columns:
            print(f"Warning: column '{col}' not present in {csv_path.name}; skipping {label}")
            continue

        mask = (df["TRH-D"] == trh_d) & (df["TREF_Freq"] == tref)
        sub = df.loc[mask, ["workload", col]].copy()
        if sub.empty:
            print(f"Warning: no rows for {label} "
                  f"(col={col}, TRH-D={trh_d}, TREF={tref})")
            continue

        sub = sub.rename(columns={col: value_col_name})
        sub["Methods"] = label
        rows.append(sub)

    df_long = pd.concat(rows, ignore_index=True)
    df_long = df_long[df_long["workload"].isin(WORKLOAD_ORDER)].copy()

    method_order = [label for _, _, label in SERIES_SPECS]
    df_long["Methods"] = pd.Categorical(df_long["Methods"],
                                        categories=method_order,
                                        ordered=True)
    return df_long, method_order


# =============================================================================
# Group-box decoration (the "Workloads with >=10 RBMPKI" / "All Workloads" boxes)
# =============================================================================
def draw_group_box(ax, transform, start_idx, end_idx, sep_x, label, side,
                   arrow_y, box_y, box_height, fontsize=11,
                   outer_extend=1.4, inner_gap=0.0, box_pad=0.35):
    """Draw a labeled arrow+box under the x-axis spanning [start_idx, end_idx].

    side:
        'left'  -> arrow's right end touches sep_x
        'right' -> arrow's left end touches sep_x
    """
    raw_start = start_idx + 0.8
    raw_end   = end_idx - 0.8

    if side == "left":
        arrow_start, arrow_end = raw_start - outer_extend, sep_x - inner_gap
    elif side == "right":
        arrow_start, arrow_end = sep_x + inner_gap, raw_end + outer_extend
    else:
        raise ValueError("side must be 'left' or 'right'")

    box_start, box_end = raw_start, raw_end
    box_center = (box_start + box_end) / 2

    ax.add_patch(FancyArrowPatch(
        posA=(arrow_start, arrow_y), posB=(arrow_end, arrow_y),
        arrowstyle="<->", mutation_scale=12, linewidth=1.2,
        transform=transform, color="black", clip_on=False, zorder=1,
    ))
    ax.add_patch(patches.FancyBboxPatch(
        (box_start + box_pad, box_y),
        (box_end - box_start) - 2 * box_pad, box_height,
        boxstyle="round,pad=0.02", linewidth=1.2,
        edgecolor="black", facecolor="white",
        transform=transform, clip_on=False, zorder=2,
    ))
    ax.text(box_center, box_y + box_height / 2, label,
            ha="center", va="center", fontsize=fontsize,
            transform=transform, zorder=3)


# =============================================================================
# Figure rendering
# =============================================================================
def render_figure(df_long, method_order, value_col, ylabel,
                  ylim, yticks, mean_label, mean_label_y,
                  out_path, draw_ellipse=False):
    """Render one bar-chart figure with the standard group-box decoration."""
    sns.set_palette("tab10")
    sns.set_style("whitegrid")
    plt.rcParams["pdf.fonttype"] = 42
    plt.rcParams["ps.fonttype"]  = 42

    fig, ax = plt.subplots(figsize=(12, 2))
    plt.rc("font", size=10)

    sns.barplot(
        x="workload", y=value_col, hue="Methods",
        data=df_long, order=WORKLOAD_ORDER, hue_order=method_order,
        edgecolor="black", ax=ax,
    )

    ax.set_xticks(np.arange(len(WORKLOAD_ORDER)))
    ax.set_xticklabels(WORKLOAD_ORDER, ha="right", rotation=45, fontsize=11)
    for tick in ax.get_xticklabels():
        if tick.get_text() in GEOMEAN_LABELS:
            tick.set_fontweight("bold")

    sep_idx = WORKLOAD_ORDER.index("")
    gmean_center = (sep_idx + 1 + len(WORKLOAD_ORDER) - 1) / 2

    ax.axvline(sep_idx, 0, 1, color="red", linestyle="--", linewidth=2)
    ax.text(gmean_center, mean_label_y, mean_label, fontweight="bold", ha="center")

    ax.tick_params(axis="x", which="major", labelsize=10.5)
    ax.tick_params(axis="y", which="major", labelsize=11.5)
    ax.set_xlabel("")
    ax.set_ylabel(ylabel, fontsize=11)

    ax.legend(loc="upper center", bbox_to_anchor=(0.5, 1.42),
              ncol=4, fancybox=True, shadow=False, fontsize=10)

    ax.set_ylim(*ylim)
    ax.set_xlim(-0.5, len(WORKLOAD_ORDER) - 0.5)
    if yticks is not None:
        ax.set_yticks(yticks)

    # Optional Fig. 7 ellipse around the "0.6" tick (visual emphasis on
    # the y-axis baseline used by the paper).
    if draw_ellipse:
        ax.add_patch(patches.Ellipse(
            (-1.1, ylim[0]), width=0.8, height=0.07,
            edgecolor="red", fill=False, clip_on=False,
            facecolor="none", linewidth=1.5,
        ))
        ax.axhline(y=1.0, color="r", linestyle="-", linewidth=2)

    # Group-box decoration under the x-axis
    transform = blended_transform_factory(ax.transData, ax.transAxes)
    arrow_y, box_y, box_height = -0.82, -0.86, 0.08

    draw_group_box(
        ax, transform,
        start_idx=0, end_idx=sep_idx - 1, sep_x=sep_idx,
        label="Workloads with \u2265 10 Row-Buffer Misses per Kilo-Instruction",
        side="left", arrow_y=arrow_y, box_y=box_y, box_height=box_height,
        outer_extend=1.4, inner_gap=0.0, box_pad=0.25,
    )
    draw_group_box(
        ax, transform,
        start_idx=sep_idx + 1, end_idx=len(WORKLOAD_ORDER) - 1, sep_x=sep_idx,
        label="All Workloads",
        side="right", arrow_y=arrow_y, box_y=box_y, box_height=box_height,
        outer_extend=1.4, inner_gap=0.0, box_pad=0.35,
    )

    plt.grid(True, linestyle=":")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=600, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {out_path}")


# =============================================================================
# Driver
# =============================================================================
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Render Figures 7 and 8 of the PrISM paper.")
    p.add_argument("--results-dir", type=str, default=str(DEFAULT_RESULTS_DIR),
                   help="Directory containing perf_normalized.csv and rfm_per_trefi.csv.")
    p.add_argument("--output-dir", type=str, default=str(DEFAULT_OUTPUT_DIR),
                   help="Directory to write the PDFs.")
    p.add_argument("--tref", type=int, default=DEFAULT_TREF,
                   help=f"TREF_Freq filter (default {DEFAULT_TREF}).")
    return p.parse_args()


def main() -> None:
    args = parse_args()
    results_dir = Path(args.results_dir)
    output_dir = Path(args.output_dir)

    # -------- Figure 7: normalized performance --------
    perf_df, method_order = build_plot_df(
        results_dir / "perf_normalized.csv",
        value_col_name="WS", tref=args.tref,
    )
    render_figure(
        perf_df, method_order,
        value_col="WS",
        ylabel="Normalized Performance",
        ylim=(0.6, 1.05),
        yticks=[0.6, 0.7, 0.8, 0.9, 1.0],
        mean_label="GMEAN", mean_label_y=1.02,
        out_path=output_dir / "fig7_perf.pdf",
        draw_ellipse=True,
    )

    # -------- Figure 8: RFMs per tREFI --------
    rfm_df, method_order = build_plot_df(
        results_dir / "rfm_per_trefi.csv",
        value_col_name="RFM", tref=args.tref,
    )
    # Find a sensible AMEAN-label y position from the data.
    ymax = rfm_df["RFM"].max()
    mean_label_y = ymax * 0.85 if pd.notna(ymax) and ymax > 0 else 7
    render_figure(
        rfm_df, method_order,
        value_col="RFM",
        ylabel="RFMs per tREFI per Channel",
        ylim=(0, max(ymax * 1.05, 1) if pd.notna(ymax) else 12),
        yticks=None,  # auto
        mean_label="AMEAN", mean_label_y=mean_label_y,
        out_path=output_dir / "fig8_rfm.pdf",
        draw_ellipse=False,
    )


if __name__ == "__main__":
    main()
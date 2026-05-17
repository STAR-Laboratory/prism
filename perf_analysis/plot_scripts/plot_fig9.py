"""
Render Figure 9 of the PrISM paper.

  Figure 9 -- Sensitivity to Target Row Refresh (TRR) rate at TRH-D = 500.
              Compares PRAC, MINT+RFM24, and PrISM across TREF_Freq values
              {1, 2, 4, 8, 0}, where 0 = no TRR (RFM-only).

The input CSV is the condensed (workload, TRH-D, TREF_Freq) format from
collate.py, with MINT/QPRAC/PrISM as side-by-side columns. PrISM is at the
default PMQ size (16) already baked into the CSV.

Inputs (default ../results/collated/):
  perf_normalized.csv

Outputs (default ../plots/):
  fig9_trr_sensitivity.pdf
"""

import argparse
from pathlib import Path

import matplotlib.patches as patches
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import seaborn as sns


# =============================================================================
# Defaults / Constants
# =============================================================================
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_RESULTS_DIR = SCRIPT_DIR.parent / "results" / "collated"
DEFAULT_OUTPUT_DIR  = SCRIPT_DIR.parent / "plots"

# Figure-9 specific filters.
DEFAULT_WORKLOAD = "All (57)"
DEFAULT_TRH_D    = 500

# X-axis: TRR rate (one TRR per X tREFIs). 0 = no TRR.
X_TICKS = [1, 2, 4, 8, 0]
X_TICK_LABELS = {1: "1", 2: "2", 4: "4", 8: "8", 0: "No"}

# (mitigation_column, plot_label)
# At TRH-D=500, MINT uses one RFM per 24 activations.
SERIES_SPECS = [
    ("QPRAC", "PRAC"),
    ("MINT",  "MINT+RFM24"),
    ("PrISM", "PrISM"),
]

# Per-method colors. Pinned indices keep colors stable across figures.
PALETTE = sns.color_palette("tab10")
METHOD_COLORS = {
    "PRAC":        PALETTE[0],
    "MINT+RFM24":  PALETTE[2],
    "PrISM":       PALETTE[5],
}


# =============================================================================
# Data prep
# =============================================================================
def build_plot_df(csv_path: Path, workload: str, trh_d: int) -> pd.DataFrame:
    """Return a long-form DataFrame indexed by (Mitigations, TREF_Freq, WS)."""
    if not csv_path.exists():
        raise FileNotFoundError(
            f"Could not find {csv_path}. Run collate.py first.")
    df = pd.read_csv(csv_path)

    required = {"workload", "TRH-D", "TREF_Freq"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing required columns in {csv_path}: {missing}")

    rows = []
    for col, label in SERIES_SPECS:
        if col not in df.columns:
            print(f"Warning: column '{col}' not in {csv_path.name}; skipping {label}")
            continue

        mask = (
            (df["workload"] == workload)
            & (df["TRH-D"] == trh_d)
            & (df["TREF_Freq"].isin(X_TICKS))
        )
        sub = df.loc[mask, ["TREF_Freq", col]].copy()
        if sub.empty:
            print(f"Warning: no rows for {label} "
                  f"(col={col}, workload={workload}, TRH-D={trh_d})")
            continue

        sub = sub.rename(columns={col: "WS"})
        sub["Mitigations"] = label
        rows.append(sub.dropna(subset=["WS"]))

    if not rows:
        raise ValueError("No data matched the selected filters.")

    df_long = pd.concat(rows, ignore_index=True)

    # Collapse any duplicates (shouldn't normally happen, but harmless).
    df_long = (df_long
               .groupby(["Mitigations", "TREF_Freq"], as_index=False)["WS"]
               .mean())
    return df_long


# =============================================================================
# Figure rendering
# =============================================================================
def render_figure(df_long: pd.DataFrame, methods_in_order, out_path: Path) -> None:
    sns.set_palette("tab10")
    sns.set_style("whitegrid")
    plt.rcParams["pdf.fonttype"] = 42
    plt.rcParams["ps.fonttype"]  = 42

    fig, ax = plt.subplots(figsize=(8, 2))
    plt.rc("font", size=12)

    bar_width = 0.12
    num_bars = len(methods_in_order)
    x_tick_positions = np.arange(len(X_TICKS), dtype=float)

    # Compute per-group bar positions
    bar_positions = {
        tick: [base - (bar_width * num_bars) / 2 + j * bar_width
               for j in range(num_bars)]
        for tick, base in zip(X_TICKS, x_tick_positions)
    }

    for tick in X_TICKS:
        sub = df_long[df_long["TREF_Freq"] == tick]
        for i, method in enumerate(methods_in_order):
            row = sub[sub["Mitigations"] == method]
            if row.empty:
                continue
            value = row["WS"].iloc[0]
            x_pos = bar_positions[tick][i] + bar_width / 2
            ax.bar(
                x_pos, value,
                width=bar_width,
                color=METHOD_COLORS.get(method, "gray"),
                edgecolor="black",
                label=method if tick == X_TICKS[0] else "",
            )

    # Vertical separators between TREF_Freq groups
    for i in range(len(X_TICKS) - 1):
        ax.axvline(x=i + 0.5, color="grey", linestyle="-", alpha=0.5)

    # Single-instance legend (dedupe labels accumulated above)
    handles, labels = ax.get_legend_handles_labels()
    seen = dict(zip(labels, handles))
    ax.legend(
        seen.values(), seen.keys(),
        loc="upper center", bbox_to_anchor=(0.5, 1.25),
        ncol=4, fancybox=True, shadow=False, fontsize=11,
    )

    ax.set_xticks(x_tick_positions)
    ax.set_xticklabels([X_TICK_LABELS[t] for t in X_TICKS])
    ax.set_xlabel(r"Target Row Refresh (TRR) Rate (One TRR per X $\mathrm{tREFI}$)",
                  fontsize=12)
    ax.set_ylabel("Normalized Performance", fontsize=12)
    ax.tick_params(axis="both", which="major", labelsize=12)

    ax.axhline(y=1.0, color="r", linestyle="-", linewidth=2)
    ax.set_ylim(0.8, 1.008)
    ax.set_yticks([0.8, 0.85, 0.9, 0.95, 1.0])
    ax.set_xlim(-0.5, len(X_TICKS) - 0.5)

    # Red ellipse highlighting the y-axis baseline (matches paper)
    ax.add_patch(patches.Ellipse(
        (-0.73, 0.8), width=0.34, height=0.04,
        edgecolor="red", fill=False, clip_on=False,
        facecolor="none", linewidth=1.5,
    ))

    plt.grid(True, linestyle=":")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=600, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {out_path}")


# =============================================================================
# Driver
# =============================================================================
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Render Figure 9 of the PrISM paper.")
    p.add_argument("--results-dir", type=str, default=str(DEFAULT_RESULTS_DIR),
                   help="Directory containing perf_normalized.csv.")
    p.add_argument("--output-dir", type=str, default=str(DEFAULT_OUTPUT_DIR),
                   help="Directory to write the PDF.")
    p.add_argument("--workload", type=str, default=DEFAULT_WORKLOAD,
                   help="Workload to plot (default 'All (57)').")
    p.add_argument("--trh-d", type=int, default=DEFAULT_TRH_D,
                   help=f"TRH-D value to filter on (default {DEFAULT_TRH_D}).")
    return p.parse_args()


def main() -> None:
    args = parse_args()

    df_long = build_plot_df(
        Path(args.results_dir) / "perf_normalized.csv",
        workload=args.workload, trh_d=args.trh_d,
    )

    methods_in_order = [
        label for _, label in SERIES_SPECS
        if label in set(df_long["Mitigations"])
    ]

    render_figure(df_long, methods_in_order,
                  out_path=Path(args.output_dir) / "fig9_trr_sensitivity.pdf")


if __name__ == "__main__":
    main()
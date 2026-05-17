"""
Render Figure 6 of the PrISM paper:
    Pareto frontier of TRH-D_base vs SHQ entries, swept across W in
    {72, 60, 48, 36, 24}. Lower curves mean lower TRH-D for a given SHQ size.

Note on which TRH-D is plotted
------------------------------
Figure 6 plots TRH_D_base (before the QTH + ABO_ACT margin), matching Figure 5,
so the two figures are directly comparable. The final TRH-D adds the margin.

Input  : CSV produced by `prism_circular_security_analysis.py sweep-w`
         (located under <results-dir>, default ../results relative to this script)
Output : <output-dir>/sens_W_SHQ_TRHDbase_QTH{qth}_ABO{abo}.pdf
"""

import argparse
from pathlib import Path

import matplotlib.lines as mlines
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns
from matplotlib.ticker import FixedLocator, FuncFormatter


# Storage tail above this is uninteresting; truncate for a cleaner figure.
MAX_SHQ_ENTRIES = 300

# Default I/O directories are resolved relative to the security_analysis/
# directory (one level above this script's location), so the defaults work
# regardless of where the user runs python from.
SECURITY_ANALYSIS_DIR = Path(__file__).resolve().parent.parent
DEFAULT_RESULTS_DIR = SECURITY_ANALYSIS_DIR / "results"
DEFAULT_OUTPUT_DIR  = SECURITY_ANALYSIS_DIR / "plots"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Render PrISM Figure 6.")
    p.add_argument("--results-dir", type=str, default=str(DEFAULT_RESULTS_DIR),
                   help="Directory containing the sweep-w CSV.")
    p.add_argument("--output-dir", type=str, default=str(DEFAULT_OUTPUT_DIR),
                   help="Directory to write the PDF.")
    p.add_argument("--qth", type=int, default=4,
                   help="PMQ tardiness threshold (paper default 4).")
    p.add_argument("--abo-act", type=int, default=12,
                   help="ABO slack activations (paper default 12 for PMQ=16).")
    p.add_argument("--w-values", type=str, default="72,60,48,36,24",
                   help="Comma-separated W values to plot, from largest to smallest.")
    p.add_argument("--save-frontier-csv", action="store_true",
                   help="Also write the frontier points to CSV for inspection.")
    return p.parse_args()


def build_frontier(df_w: pd.DataFrame) -> pd.DataFrame:
    """Pareto frontier on (SHQ_entries, TRH_D_base): keep only points where
    increasing SHQ_entries strictly improves (lowers) TRH_D_base.
    """
    # For ties on SHQ_entries, pick the lowest TRH-D, then smallest R, L.
    grouped = (
        df_w.sort_values(["SHQ_entries", "TRH_D_base", "R", "L"])
            .groupby("SHQ_entries", as_index=False)
            .first()
            .sort_values("SHQ_entries")
            .reset_index(drop=True)
    )

    frontier = []
    best_so_far = float("inf")
    for _, row in grouped.iterrows():
        if row["TRH_D_base"] < best_so_far:
            frontier.append(row)
            best_so_far = row["TRH_D_base"]
    return (pd.DataFrame(frontier).reset_index(drop=True)
            if frontier else pd.DataFrame(columns=grouped.columns))


def main() -> None:
    args = parse_args()

    plt.rcParams["pdf.fonttype"] = 42
    plt.rcParams["ps.fonttype"] = 42
    plt.rcParams["font.size"] = 11
    sns.set_palette("tab10")
    sns.set_style("whitegrid")

    results_dir = Path(args.results_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    tag = f"QTH{args.qth}_ABO{args.abo_act}"
    csv_path = results_dir / f"sweep_W_R_L_{tag}.csv"
    if not csv_path.exists():
        raise FileNotFoundError(
            f"Could not find {csv_path}. Run the 'sweep-w' subcommand first.")
    df = pd.read_csv(csv_path)

    required = {"W", "R", "L", "SHQ_entries", "TRH_D_base"}
    missing = required - set(df.columns)
    if missing:
        raise ValueError(f"Missing required columns: {missing}")

    w_values = [int(x.strip()) for x in args.w_values.split(",") if x.strip()]
    df = df[df["W"].isin(w_values)].copy()

    # (R=2, L=2) collapses PrISM to a MINT-equivalent design point. Exclude it
    # so the frontier reflects only true PrISM configurations.
    df = df[~((df["R"] == 2) & (df["L"] == 2))].copy()

    # ---- Frontiers per W ----
    frontiers = {}
    for W in w_values:
        sub = df[df["W"] == W]
        if sub.empty:
            continue
        f = build_frontier(sub)
        f = f[f["SHQ_entries"] <= MAX_SHQ_ENTRIES]
        frontiers[W] = f

    # ---- Optional frontier-points CSV ----
    if args.save_frontier_csv:
        rows = []
        preferred_cols = [
            "W", "R", "L", "SHQ_entries", "TRH_D_base", "TRH_D", "TRH_D_margin",
            "A_star", "X_win_star", "N_per_row_star", "p_m_star",
            "sampling_fraction", "default_mitigation_rate",
        ]
        for W, f in frontiers.items():
            if f.empty:
                continue
            f = f.copy()
            f["frontier_used_in_plot"] = True
            rows.append(f)
        if rows:
            df_frontier = pd.concat(rows, ignore_index=True)
            cols = [c for c in preferred_cols if c in df_frontier.columns]
            extras = [c for c in df_frontier.columns if c not in cols]
            df_frontier = df_frontier[cols + extras]
            frontier_csv = output_dir / f"frontier_W_SHQ_TRHDbase_leq{MAX_SHQ_ENTRIES}_{tag}.csv"
            df_frontier.to_csv(frontier_csv, index=False)
            print(f"Saved frontier points: {frontier_csv}")

    # ---- Plot ----
    fig, ax = plt.subplots(figsize=(4.5, 1.8))
    colors = list(plt.get_cmap("tab10").colors)
    handles = []

    for idx, W in enumerate(w_values):
        f = frontiers.get(W)
        if f is None or f.empty:
            continue
        color = colors[idx % len(colors)]
        ax.plot(f["SHQ_entries"], f["TRH_D_base"],
                color=color, linewidth=1.7, marker="o",
                markersize=3, zorder=1)
        handles.append(mlines.Line2D([], [], color=color, linewidth=1.7,
                                     marker="o", markersize=3, label=f"W = {W}"))

    ax.set_xlabel("Sampled History Queue (SHQ) Entries", fontsize=11)
    ax.set_ylabel("Double-Sided RowHammer\nThreshold (T$_{RH-D}$)", fontsize=11)
    ax.tick_params(axis="both", which="major", labelsize=10.5)

    yticks = [125, 250, 500, 750, 1000]
    ax.yaxis.set_major_locator(FixedLocator(yticks))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{int(y)}"))
    ax.set_ylim(150, 1100)
    ax.set_xlim(-5, MAX_SHQ_ENTRIES + 10)
    ax.grid(True, linewidth=0.7, alpha=0.7)

    top_legend = ax.legend(handles=handles, loc="upper center",
                           bbox_to_anchor=(0.5, 1.55),
                           title="Mitigation Window Size (W)",
                           ncol=3, fontsize=10, frameon=True)
    ax.add_artist(top_legend)

    for spine in ax.spines.values():
        spine.set_linewidth(0.8)
        spine.set_color("0.7")

    fig_path = output_dir / f"sens_W_SHQ_TRHDbase_{tag}.pdf"
    fig.savefig(fig_path, dpi=600, bbox_inches="tight",
                bbox_extra_artists=(top_legend,), pad_inches=0.05)
    plt.close(fig)
    print(f"Saved: {fig_path}")


if __name__ == "__main__":
    main()
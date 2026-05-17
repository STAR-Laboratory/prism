"""
Render Figure 5 of the PrISM paper:
    Double-sided RowHammer threshold (TRH-D, base only) vs lookback window L,
    at fixed W=72, with analytical curves and Monte-Carlo markers.

Note on which TRH-D is plotted
------------------------------
Figure 5 shows TRH_D_base, i.e., floor(T_single / 2) *before* the post-selection
margin (QTH + ABO_ACT) is added. This isolates the analytical security
contribution from the PMQ/ABO bookkeeping margin. The final TRH-D used in the
paper's headline numbers adds (QTH + ABO_ACT) to these values.

Input  : CSV produced by `prism_circular_security_analysis.py figure8`
         (located under <results-dir>, default ../results relative to this script)
Output : <output-dir>/sens_R_L_TRHD_W{W}_QTH{qth}_ABO{abo}.pdf
"""

import argparse
from pathlib import Path

import matplotlib.lines as mlines
import matplotlib.pyplot as plt
import pandas as pd
import seaborn as sns
from matplotlib.ticker import FuncFormatter, NullFormatter, NullLocator


# Default I/O directories are resolved relative to the security_analysis/
# directory (one level above this script's location), so the defaults work
# regardless of where the user runs python from.
SECURITY_ANALYSIS_DIR = Path(__file__).resolve().parent.parent
DEFAULT_RESULTS_DIR = SECURITY_ANALYSIS_DIR / "results"
DEFAULT_OUTPUT_DIR  = SECURITY_ANALYSIS_DIR / "plots"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Render PrISM Figure 5.")
    p.add_argument("--results-dir", type=str, default=str(DEFAULT_RESULTS_DIR),
                   help="Directory containing the figure8 CSV.")
    p.add_argument("--output-dir", type=str, default=str(DEFAULT_OUTPUT_DIR),
                   help="Directory to write the PDF.")
    p.add_argument("--w", type=int, default=72, help="Mitigation window size.")
    p.add_argument("--qth", type=int, default=4,
                   help="PMQ tardiness threshold (paper default 4).")
    p.add_argument("--abo-act", type=int, default=12,
                   help="ABO slack activations (paper default 12 for PMQ=16).")
    return p.parse_args()


def main() -> None:
    args = parse_args()

    # ---- Style ----
    plt.rcParams["pdf.fonttype"] = 42
    plt.rcParams["ps.fonttype"] = 42
    plt.rcParams["font.size"] = 11
    sns.set_palette("tab10")
    sns.set_style("whitegrid")

    results_dir = Path(args.results_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    tag = f"QTH{args.qth}_ABO{args.abo_act}"
    csv_path = results_dir / f"figure8_generalized_with_mc_W{args.w}_{tag}.csv"
    if not csv_path.exists():
        raise FileNotFoundError(
            f"Could not find {csv_path}. Run the 'figure8' subcommand first.")
    df = pd.read_csv(csv_path)

    # Plot the *base* TRH-D (before the QTH + ABO_ACT margin) so the curve
    # reflects only the analytical security contribution.
    analytic_col = "TRH_D_base_analytical"
    sim_col = "TRH_D_base_simulated"
    if analytic_col not in df.columns:
        raise ValueError(f"Missing required column: {analytic_col}")
    has_mc = sim_col in df.columns

    r_values = sorted(df["R"].dropna().unique())
    l_min, l_max = df["L"].min(), df["L"].max()

    # ---- Plot ----
    fig, ax = plt.subplots(figsize=(4.5, 1.8))
    colors = list(plt.get_cmap("tab10").colors)
    handles = []

    for idx, R in enumerate(r_values):
        sub = df[df["R"] == R].sort_values("L")
        if sub.empty:
            continue
        color = colors[idx % len(colors)]

        ax.plot(sub["L"], sub[analytic_col], color=color, linewidth=1.6, zorder=1)
        handles.append(mlines.Line2D([], [], color=color, linewidth=1.6,
                                     label=f"R = {int(R)}"))

        if has_mc:
            mc_sub = sub.dropna(subset=[sim_col])
            if not mc_sub.empty:
                ax.plot(mc_sub["L"], mc_sub[sim_col],
                        marker="o", markersize=4.8, markerfacecolor="none",
                        markeredgecolor=color, markeredgewidth=0.7,
                        linestyle="None", zorder=2)

    # ---- Axes ----
    ax.set_xlabel("Lookback Window (L)", fontsize=11)
    ax.set_ylabel("Double-Sided RowHammer\nThreshold (T$_{RH-D}$)", fontsize=11)
    ax.tick_params(axis="both", which="major", labelsize=11)

    ax.set_yticks([500, 750, 1000, 1250])
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{int(y)}"))
    ax.yaxis.set_minor_formatter(NullFormatter())
    ax.yaxis.set_minor_locator(NullLocator())
    ax.tick_params(axis="y", which="minor", left=False)
    ax.set_ylim(350, 1400)
    ax.set_xlim(l_min - 0.5, l_max + 0.5)
    ax.grid(True, linewidth=0.7, alpha=0.7)

    for spine in ax.spines.values():
        spine.set_linewidth(0.8)
        spine.set_color("0.7")

    # ---- Legends ----
    top_legend = ax.legend(handles=handles, loc="upper center",
                           bbox_to_anchor=(0.5, 1.55),
                           title="Sampled Activation Slots (R)",
                           ncol=4, fontsize=9, frameon=True)
    ax.add_artist(top_legend)

    if has_mc:
        ax.legend(handles=[mlines.Line2D([], [], color="black", marker="o",
                                         markerfacecolor="none",
                                         markeredgewidth=0.9,
                                         linestyle="None", label="Simulation")],
                  loc="best", fontsize=9.5, frameon=True, framealpha=0.9)

    # ---- Save ----
    fig_path = output_dir / f"sens_R_L_TRHD_W{args.w}_{tag}.pdf"
    fig.savefig(fig_path, dpi=600, bbox_inches="tight",
                bbox_extra_artists=(top_legend,), pad_inches=0.05)
    plt.close(fig)
    print(f"Saved: {fig_path}")


if __name__ == "__main__":
    main()
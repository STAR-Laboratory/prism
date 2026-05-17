"""
Generate Table VI of the PrISM paper.

  Table VI -- Impact of PMQ size on ABO_ACT(Q) and performance overhead
              at TRH-D = 500 (default TREF = 2).

Columns
-------
  PMQ Size              from --pmq-sizes
  ABO_ACT(Q)            analytical (Section IV-B of the paper)
  Performance Overhead  from pmq_sweep.csv (1 - normalized speedup)

Inputs (default ../results/collated/):
  pmq_sweep.csv         (PrISM-only sweep across PMQ in {4, 8, 16, 32} at
                         TRH-D=500, TREF=2; columns are workload, PMQ4,
                         PMQ8, PMQ16, PMQ32)

Outputs (default ../tables/):
  table6_pmq_sensitivity.txt   (plain text, for inspection / README)
"""

import argparse
from pathlib import Path

import pandas as pd


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_RESULTS_DIR = SCRIPT_DIR.parent / "results" / "collated"
DEFAULT_OUTPUT_DIR  = SCRIPT_DIR.parent / "tables"

DEFAULT_PMQ_SIZES = [4, 8, 16, 32]
DEFAULT_WORKLOAD  = "All (57)"
DEFAULT_HIGHLIGHT = 16   # PMQ size marked as the paper default

# Worst-case slack activations during chained ABO, per Section IV-B of the
# PrISM paper. Add new entries here if you sweep additional PMQ sizes.
ABO_ACT_TABLE = {
    4:  7,
    8:  10,
    16: 12,
    32: 14,
}


def abo_act_for_pmq(q: int) -> int:
    if q not in ABO_ACT_TABLE:
        raise ValueError(
            f"ABO_ACT(Q={q}) not in the published table. "
            f"Known PMQ sizes: {sorted(ABO_ACT_TABLE.keys())}. "
            f"Add the value to ABO_ACT_TABLE if you've derived it for a new PMQ size."
        )
    return ABO_ACT_TABLE[q]


def collect_overheads(csv_path: Path, pmq_sizes, workload: str) -> dict:
    """Return {pmq_size: overhead_percent} from pmq_sweep.csv."""
    if not csv_path.exists():
        raise FileNotFoundError(
            f"Could not find {csv_path}. Run collate.py first.")
    df = pd.read_csv(csv_path)

    if "workload" not in df.columns:
        raise ValueError(f"Missing 'workload' column in {csv_path}")

    pmq_cols = [f"PMQ{p}" for p in pmq_sizes]
    missing = set(pmq_cols) - set(df.columns)
    if missing:
        raise ValueError(f"Missing PMQ columns in {csv_path}: {missing}")

    row = df.loc[df["workload"] == workload]
    if row.empty:
        raise ValueError(
            f"Workload '{workload}' not found in {csv_path}. "
            f"Available workloads: {df['workload'].unique().tolist()[:10]}...")

    overheads = {}
    for pmq, col in zip(pmq_sizes, pmq_cols):
        value = row[col].iloc[0]
        if pd.isna(value):
            print(f"Warning: no PrISM value for workload={workload}, PMQ={pmq}")
            overheads[pmq] = None
        else:
            overheads[pmq] = (1.0 - float(value)) * 100.0
    return overheads


def format_overhead(value) -> str:
    return f"{value:.1f}%" if value is not None else "--"


def render_table(pmq_sizes, overheads, highlight_pmq: int) -> str:
    """Render an aligned plain-text table."""
    header = ("PMQ Size", "ABO_ACT(Q)", "Perf. Overhead")
    rows = []
    for pmq in pmq_sizes:
        abo = abo_act_for_pmq(pmq)
        oh = format_overhead(overheads.get(pmq))
        marker = " *" if pmq == highlight_pmq else "  "
        rows.append((f"{pmq}{marker}", str(abo), oh))

    col_widths = [
        max(len(header[i]), max(len(r[i]) for r in rows))
        for i in range(3)
    ]
    sep = "+-" + "-+-".join("-" * w for w in col_widths) + "-+"
    fmt = "| " + " | ".join(f"{{:<{w}}}" for w in col_widths) + " |"

    out = [sep, fmt.format(*header), sep]
    for r in rows:
        out.append(fmt.format(*r))
    out.append(sep)
    out.append("")
    out.append("(* = paper default)")
    return "\n".join(out) + "\n"


def main():
    p = argparse.ArgumentParser(description="Generate Table VI of the PrISM paper.")
    p.add_argument("--results-dir", type=str, default=str(DEFAULT_RESULTS_DIR),
                   help="Directory containing pmq_sweep.csv.")
    p.add_argument("--output-dir", type=str, default=str(DEFAULT_OUTPUT_DIR),
                   help="Directory to write the table file.")
    p.add_argument("--pmq-sizes", type=str,
                   default=",".join(str(x) for x in DEFAULT_PMQ_SIZES),
                   help="Comma-separated PMQ sizes (default 4,8,16,32).")
    p.add_argument("--workload", type=str, default=DEFAULT_WORKLOAD,
                   help="Workload to summarize (default 'All (57)').")
    p.add_argument("--highlight-pmq", type=int, default=DEFAULT_HIGHLIGHT,
                   help=f"PMQ size to mark with * (default {DEFAULT_HIGHLIGHT}).")
    args = p.parse_args()

    pmq_sizes = [int(x.strip()) for x in args.pmq_sizes.split(",") if x.strip()]
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    overheads = collect_overheads(
        Path(args.results_dir) / "pmq_sweep.csv",
        pmq_sizes=pmq_sizes,
        workload=args.workload,
    )

    table = render_table(pmq_sizes, overheads, args.highlight_pmq)

    out_path = output_dir / "table6_pmq_sensitivity.txt"
    out_path.write_text(table)

    print(table)
    print(f"Saved: {out_path}")


if __name__ == "__main__":
    main()
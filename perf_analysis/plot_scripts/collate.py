"""
Collate Ramulator2 result files into canonical CSVs for plotting.

Walks <results-dir>/<mitigation>/stats/*.txt for each configured mitigation,
extracts per-run weighted speedup and RFM frequency, normalizes performance
against Baseline, and writes the following canonical CSVs:

  1. baseline_mpki.csv     -- per-workload LLC/RB MPKI from Baseline.
  2. perf_normalized.csv   -- per-workload weighted speedup normalized to
                              Baseline, indexed by (workload, TRH-D, TREF),
                              with MINT/QPRAC/PrISM side-by-side. PrISM
                              uses the default PMQ size (16). Source for
                              Figs. 7, 8 (in part), and 9.
  3. rfm_per_trefi.csv     -- per-workload average RFMs per tREFI, same
                              indexing as (2). Source for Fig. 8.
  4. pmq_sweep.csv         -- PrISM-only performance across PMQ sizes
                              {4, 8, 16, 32} at the paper's default
                              (TRH-D=500, TREF=2). Source for Table VI.

CSVs (2) and (3) contain both per-workload rows and per-suite / 'All (57)'
summary rows (geomean for performance, arithmetic mean for RFM).

Filename conventions
--------------------
  Baseline    : 8000_<workload>
  Non-PrISM   : 8000_<NRH>_<TREF>_<workload>            (or with leading
                                                         "<mitigation>_" prefix)
  PrISM       : 8000_<NRH>_<TREF>_PMQ<size>_<workload>  (or with leading
                                                         "PrISM_" prefix)

Some setup_run.py versions prepend the mitigation name to the per-job result
filename (e.g., "MINT_8000_500_2_429.mcf.txt"). The parser strips that prefix
if present, so both naming styles work.

Naming note: the internal column is "NRH" (the Ramulator2-side parameter), but
the user-facing CSVs export it as "TRH-D" to match paper terminology.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd


# =============================================================================
# Defaults
# =============================================================================
DEFAULT_NUM_CORES = 8
DEFAULT_INTERFACE = 8000

VALID_TREF_FREQS = {0, 1, 2, 4, 8}
VALID_NRH = {250, 500, 1000}

# All mitigations the script knows how to parse. Mitigations not present on
# disk are skipped with a warning.
MITIGATIONS = ["Baseline", "MINT", "QPRAC", "PrISM"]

# Mitigations to include as columns in the per-workload pivots and summaries.
MITIGATIONS_NON_BASELINE = ["QPRAC", "MINT", "PrISM"]

# Paper defaults used by the condensed perf / rfm CSVs and the PMQ-sweep CSV.
DEFAULT_PMQ = 16
PMQ_SWEEP_NRH = 500
PMQ_SWEEP_TREF = 2


# =============================================================================
# Benchmark suites and category groupings
# =============================================================================
BENCHMARK_SUITES = {
    "HIGH (17)": [
        "429.mcf", "470.lbm", "434.zeusmp", "519.lbm", "549.fotonik3d",
        "459.GemsFDTD", "450.soplex", "462.libquantum", "433.milc",
        "437.leslie3d", "510.parest", "520.omnetpp", "483.xalancbmk",
        "wc_8443", "wc_map0", "482.sphinx3", "tpch2",
    ],
    "MEDIUM (21)": [
        "471.omnetpp", "grep_map0", "473.astar", "505.mcf", "tpch17",
        "436.cactusADM", "jp2_decode", "507.cactuBSSN", "557.xz", "tpcc64",
        "jp2_encode", "ycsb_aserver", "ycsb_eserver", "ycsb_bserver",
        "ycsb_cserver", "ycsb_dserver", "500.perlbench", "523.xalancbmk",
        "tpch6", "ycsb_abgsave", "456.hmmer",
    ],
    "LOW (19)": [
        "401.bzip2", "502.gcc", "435.gromacs", "458.sjeng", "445.gobmk",
        "525.x264", "508.namd", "531.deepsjeng", "544.nab", "526.blender",
        "403.gcc", "464.h264ref", "h264_encode", "447.dealII", "444.namd",
        "481.wrf", "541.leela", "538.imagick", "511.povray",
    ],
    "SPEC2K6 (23)": [
        "401.bzip2", "403.gcc", "429.mcf", "433.milc", "434.zeusmp",
        "435.gromacs", "436.cactusADM", "437.leslie3d", "444.namd",
        "445.gobmk", "447.dealII", "450.soplex", "456.hmmer",
        "458.sjeng", "459.GemsFDTD", "462.libquantum", "464.h264ref",
        "470.lbm", "471.omnetpp", "473.astar", "481.wrf",
        "482.sphinx3", "483.xalancbmk",
    ],
    "SPEC2K17 (18)": [
        "500.perlbench", "502.gcc", "505.mcf", "507.cactuBSSN",
        "508.namd", "510.parest", "511.povray", "519.lbm",
        "520.omnetpp", "523.xalancbmk", "525.x264", "526.blender",
        "531.deepsjeng", "538.imagick", "541.leela", "544.nab",
        "549.fotonik3d", "557.xz",
    ],
    "TPC (4)": ["tpcc64", "tpch17", "tpch2", "tpch6"],
    "Hadoop (3)": ["grep_map0", "wc_8443", "wc_map0"],
    "MediaBench (3)": ["h264_encode", "jp2_decode", "jp2_encode"],
    "YCSB (6)": [
        "ycsb_abgsave", "ycsb_aserver", "ycsb_bserver",
        "ycsb_cserver", "ycsb_dserver", "ycsb_eserver",
    ],
}

ALL_REAL_WORKLOADS = sorted({w for ws in BENCHMARK_SUITES.values() for w in ws})


# =============================================================================
# Small helpers
# =============================================================================
def safe_int(text, default: int = 0) -> int:
    try:
        return int(str(text).strip())
    except Exception:
        return default


def parse_result_name(filename: str, mitigation: str
                      ) -> tuple[int, Optional[int], Optional[int], Optional[int], Optional[str]]:
    """Parse a Ramulator2 result-file basename.

    Returns (interface, NRH, TREF, PMQ_capacity, workload). Any unparseable
    field is returned as None. For Baseline, NRH/TREF/PMQ are always None.
    For PrISM, PMQ is set; for other mitigations, PMQ is None.
    """
    parts = filename.split("_")

    # Some setup_run.py versions prepend the mitigation name to the result
    # filename (e.g., "MINT_8000_500_2_429.mcf"). The mitigation is already
    # encoded in the parent directory, so strip the prefix if present.
    if parts and parts[0] == mitigation:
        parts = parts[1:]

    interface = safe_int(parts[0], default=-1)

    if mitigation == "Baseline":
        workload = "_".join(parts[1:]) if len(parts) > 1 else None
        return interface, None, None, None, workload

    # Non-baseline filename: 8000_<NRH>_<TREF>[_PMQ<size>]_<workload>
    try:
        nrh = int(parts[1])
        tref_freq = int(parts[2])
    except (IndexError, ValueError):
        return interface, None, None, None, None

    # PrISM uses an extra PMQ<size> token
    pmq_capacity = None
    next_idx = 3
    if mitigation == "PrISM":
        if len(parts) <= 3 or not parts[3].startswith("PMQ"):
            # Malformed PrISM filename
            return interface, nrh, tref_freq, None, None
        try:
            pmq_capacity = int(parts[3][len("PMQ"):])
        except ValueError:
            return interface, nrh, tref_freq, None, None
        next_idx = 4

    workload = "_".join(parts[next_idx:]) if len(parts) > next_idx else None
    return interface, nrh, tref_freq, pmq_capacity, workload


def calculate_geometric_mean(series: pd.Series) -> float:
    valid = series.dropna()
    valid = valid[valid > 0]
    if valid.empty:
        return np.nan
    return float(np.prod(valid) ** (1 / len(valid)))


def calculate_arithmetic_mean(series: pd.Series) -> float:
    valid = series.dropna()
    return float(valid.mean()) if not valid.empty else np.nan


def drop_empty_value_rows(df: pd.DataFrame, value_cols: list[str]) -> pd.DataFrame:
    """Drop rows where every value column is NaN.

    The pivot table generates Cartesian-product rows across the grouping keys
    (e.g., every NRH x TREF combination), but most are unused at any given
    NRH x TREF. Removing those keeps the canonical CSV to actual data only.
    """
    if df.empty or not value_cols:
        return df
    cols = [c for c in value_cols if c in df.columns]
    if not cols:
        return df
    mask = df[cols].notna().any(axis=1)
    return df.loc[mask].reset_index(drop=True)


# =============================================================================
# Per-run parsing
# =============================================================================
def parse_run_stats(stats_path: Path, num_cores: int, is_baseline: bool) -> Optional[dict]:
    """Parse one Ramulator2 stats file and return the per-run metrics dict, or
    None if the run is invalid (e.g., all-zero cycle counts).
    """
    cycles = [0] * num_cores
    insts = [0] * num_cores
    num_trefi_period = 0
    num_rfm_reqs = 0
    llc_read_misses = 0
    llc_write_misses = 0
    rb_misses = 0

    with open(stats_path, "r", errors="ignore") as f:
        for line in f:
            for i in range(num_cores):
                if f" cycles_recorded_core_{i}:" in line:
                    cycles[i] = safe_int(line.split(":")[-1])
                elif f" insts_recorded_core_{i}" in line:
                    insts[i] = safe_int(line.split(":")[-1])

            if " controller0_num_refresh_reqs" in line:
                num_trefi_period = safe_int(line.split(":")[-1])
            elif " controller0_num_rfm_reqs" in line:
                num_rfm_reqs = safe_int(line.split(":")[-1])

            if is_baseline:
                if "llc_read_misses:" in line:
                    llc_read_misses = safe_int(line.split(":")[-1])
                elif "llc_write_misses:" in line:
                    llc_write_misses = safe_int(line.split(":")[-1])
                elif "controller0_num_row_misses:" in line:
                    rb_misses = safe_int(line.split(":")[-1])

    if all(c == 0 for c in cycles):
        return None

    if any(c == 0 for c in cycles):
        print(f"  Warning: at least one core has zero cycles: {stats_path.name}")

    ipcs = [insts[i] / cycles[i] if cycles[i] > 0 else 0.0 for i in range(num_cores)]
    ws = sum(ipcs)
    total_insts = sum(insts)

    metrics = {
        "WS": ws,
        "RFM_per_tREFI": num_rfm_reqs / num_trefi_period if num_trefi_period > 0 else 0.0,
    }
    if is_baseline:
        metrics["LLC_MPKI"] = (llc_read_misses + llc_write_misses) / (total_insts / 1000.0) if total_insts > 0 else 0.0
        metrics["RB_MPKI"] = rb_misses / (total_insts / 1000.0) if total_insts > 0 else 0.0
    return metrics


# =============================================================================
# Top-level walk
# =============================================================================
def collect_results(results_dir: Path, num_cores: int, interface: int
                    ) -> tuple[pd.DataFrame, pd.DataFrame]:
    """Walk results_dir/<mitigation>/stats/*.txt for each known mitigation.

    Returns
    -------
    (baseline_df, runs_df)
        baseline_df : columns workload, WS, LLC_MPKI, RB_MPKI
        runs_df     : columns workload, mitigation, NRH, TREF_Freq,
                      PMQ_Capacity, WS, RFM_per_tREFI
    """
    baseline_rows = []
    run_rows = []

    for mitigation in MITIGATIONS:
        stats_dir = results_dir / mitigation / "stats"
        if not stats_dir.exists():
            print(f"Warning: results directory not found: {stats_dir}")
            continue

        is_baseline = (mitigation == "Baseline")
        print(f"Parsing {mitigation} ...")

        for entry in sorted(os.listdir(stats_dir)):
            if not entry.endswith(".txt"):
                continue
            basename = entry[:-4]

            iface, nrh, tref, pmq, workload = parse_result_name(basename, mitigation)
            if iface != interface or workload is None:
                continue

            if not is_baseline:
                if nrh not in VALID_NRH or tref not in VALID_TREF_FREQS:
                    continue
                if mitigation == "PrISM" and pmq is None:
                    continue

            metrics = parse_run_stats(stats_dir / entry, num_cores, is_baseline)
            if metrics is None:
                continue

            if is_baseline:
                baseline_rows.append({
                    "workload": workload,
                    "WS": metrics["WS"],
                    "LLC_MPKI": metrics["LLC_MPKI"],
                    "RB_MPKI": metrics["RB_MPKI"],
                })
            else:
                run_rows.append({
                    "workload": workload,
                    "mitigation": mitigation,
                    "NRH": nrh,
                    "TREF_Freq": tref,
                    "PMQ_Capacity": pmq,  # None for non-PrISM
                    "WS": metrics["WS"],
                    "RFM_per_tREFI": metrics["RFM_per_tREFI"],
                })

    baseline_df = pd.DataFrame(baseline_rows)
    runs_df = pd.DataFrame(run_rows)
    return baseline_df, runs_df


# =============================================================================
# Normalize and reshape to the condensed (workload, NRH, TREF) layout
# =============================================================================
def normalize_perf(runs_df: pd.DataFrame, baseline_df: pd.DataFrame,
                   clip_at_one: bool) -> pd.DataFrame:
    """Add a normalized-WS column (WS / Baseline.WS) to runs_df."""
    if runs_df.empty:
        return runs_df

    baseline_perf = baseline_df[["workload", "WS"]].rename(columns={"WS": "Baseline_WS"})
    df = runs_df.merge(baseline_perf, on="workload", how="left")
    df["WS_norm"] = df["WS"] / df["Baseline_WS"]
    if clip_at_one:
        df["WS_norm"] = df["WS_norm"].clip(upper=1)
    return df.drop(columns=["Baseline_WS"])


def build_condensed_view(runs_df: pd.DataFrame, value_col: str,
                         default_pmq: int) -> pd.DataFrame:
    """Build the condensed (workload, NRH, TREF) view with mitigation columns.

    Filters PrISM to the default PMQ size, then pivots to put each mitigation
    in its own column.
    """
    if runs_df.empty:
        return pd.DataFrame()

    # Keep MINT/QPRAC (no PMQ) and PrISM only at the default PMQ size.
    mask = runs_df["PMQ_Capacity"].isna() | (runs_df["PMQ_Capacity"] == default_pmq)
    sub = runs_df.loc[mask].copy()

    return (
        sub.pivot_table(
            index=["workload", "NRH", "TREF_Freq"],
            columns="mitigation",
            values=value_col,
            aggfunc="first",
            dropna=False,
        )
        .reset_index()
    )


def build_pmq_sweep(runs_df: pd.DataFrame, nrh: int, tref: int) -> pd.DataFrame:
    """PrISM normalized WS across all PMQ sizes at the paper's NRH/TREF setting."""
    if runs_df.empty:
        return pd.DataFrame()

    sub = runs_df[
        (runs_df["mitigation"] == "PrISM")
        & (runs_df["NRH"] == nrh)
        & (runs_df["TREF_Freq"] == tref)
        & (runs_df["PMQ_Capacity"].notna())
    ].copy()

    if sub.empty:
        return pd.DataFrame()

    df = (
        sub.pivot_table(
            index="workload",
            columns="PMQ_Capacity",
            values="WS_norm",
            aggfunc="first",
            dropna=False,
        )
        .reset_index()
    )
    df.columns.name = None  # drop the pivot's column-axis name

    # Rename PMQ columns from "4" -> "PMQ4", "8" -> "PMQ8", etc.
    rename = {c: f"PMQ{int(c)}" for c in df.columns if c != "workload"}
    df = df.rename(columns=rename)

    # Order PMQ columns numerically.
    pmq_cols = sorted(
        [c for c in df.columns if c.startswith("PMQ")],
        key=lambda c: int(c[3:]),
    )
    return df[["workload"] + pmq_cols]


# =============================================================================
# Summary rows (per-suite geomean / arithmetic mean)
# =============================================================================
def add_summary_rows(df: pd.DataFrame, metric_type: str,
                     value_cols: list[str]) -> pd.DataFrame:
    """Append per-suite and 'All (57)' summary rows for the given value columns.

    Works for both the (workload, NRH, TREF) condensed view and the (workload,)
    PMQ-sweep view. Grouping keys are auto-detected from the dataframe.
    """
    if df.empty:
        return df

    aggregator = calculate_geometric_mean if metric_type == "perf" else calculate_arithmetic_mean

    # Detect grouping keys: everything that isn't a value column or "workload".
    group_cols = [c for c in df.columns if c not in value_cols and c != "workload"]

    # Build (group_key_tuple -> dict) for per-suite and "All (57)" rows.
    summary_rows = []

    if group_cols:
        observed = df[group_cols].drop_duplicates().sort_values(group_cols)
        group_iter = (tuple(row) for _, row in observed.iterrows())
    else:
        group_iter = iter([()])  # one synthetic group

    for group_key in group_iter:
        # Build the mask for this group.
        if group_cols:
            mask = pd.Series(True, index=df.index)
            for col, val in zip(group_cols, group_key):
                if pd.isna(val):
                    mask &= df[col].isna()
                else:
                    mask &= (df[col] == val)
        else:
            mask = pd.Series(True, index=df.index)

        sub_for_group = df[mask]

        # Per-suite rows
        for suite_name, workloads in BENCHMARK_SUITES.items():
            suite_df = sub_for_group[sub_for_group["workload"].isin(workloads)]
            if suite_df.empty:
                continue
            row = {"workload": suite_name}
            for col, val in zip(group_cols, group_key):
                row[col] = val
            for vc in value_cols:
                if vc in suite_df.columns:
                    row[vc] = aggregator(suite_df[vc])
            summary_rows.append(row)

        # "All (57)" row
        all_df = sub_for_group[sub_for_group["workload"].isin(ALL_REAL_WORKLOADS)]
        if not all_df.empty:
            row = {"workload": "All (57)"}
            for col, val in zip(group_cols, group_key):
                row[col] = val
            for vc in value_cols:
                if vc in all_df.columns:
                    row[vc] = aggregator(all_df[vc])
            summary_rows.append(row)

    if not summary_rows:
        return df
    return pd.concat([df, pd.DataFrame(summary_rows)], ignore_index=True)


# =============================================================================
# Final column ordering & sort
# =============================================================================
def format_and_sort(df: pd.DataFrame, value_cols: list[str]) -> pd.DataFrame:
    """Final column ordering and sort for the condensed view."""
    if df.empty:
        return df

    group_cols = [c for c in df.columns if c not in value_cols and c != "workload"]
    desired_cols = ["workload"] + group_cols + value_cols
    df = df[[c for c in desired_cols if c in df.columns]].copy()

    # Coerce grouping columns to nullable integers where applicable.
    for col in group_cols:
        if col in {"NRH", "TREF_Freq"}:
            df[col] = pd.to_numeric(df[col], errors="coerce").astype("Int64")

    # TREF ordering (1, 2, 4, 8, then 0 = "no TRR").
    if "TREF_Freq" in df.columns:
        tref_order = {1: 0, 2: 1, 4: 2, 8: 3, 0: 4}
        df["_tref_order"] = df["TREF_Freq"].map(tref_order).fillna(99)
    else:
        df["_tref_order"] = 0

    geomean_workloads = list(BENCHMARK_SUITES.keys()) + ["All (57)"]
    geomean_set = set(geomean_workloads)
    df["_is_geomean"] = df["workload"].isin(geomean_set).astype(int)

    real_workloads = sorted(set(df["workload"]) - geomean_set)
    real_order = {w: i for i, w in enumerate(real_workloads)}
    geomean_order = {w: i for i, w in enumerate(geomean_workloads)}
    df["_workload_order"] = df["workload"].map(
        lambda w: geomean_order[w] if w in geomean_set else real_order.get(w, 999)
    )

    sort_cols = ["_is_geomean", "_workload_order"]
    if "NRH" in df.columns:
        sort_cols.append("NRH")
    sort_cols.append("_tref_order")

    df = (
        df.sort_values(sort_cols)
          .drop(columns=["_is_geomean", "_workload_order", "_tref_order"])
          .reset_index(drop=True)
    )
    return df


# =============================================================================
# Driver
# =============================================================================
def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Collate Ramulator2 results into canonical CSVs.")
    p.add_argument("--results-dir", type=str, default="../results",
                   help="Directory containing per-mitigation Ramulator2 outputs.")
    p.add_argument("--output-dir", type=str, default="../results/collated",
                   help="Directory to write the canonical CSVs.")
    p.add_argument("--num-cores", type=int, default=DEFAULT_NUM_CORES,
                   help="Number of cores in the simulation (default 8).")
    p.add_argument("--interface", type=int, default=DEFAULT_INTERFACE,
                   help="DDR5 interface speed to include (default 8000).")
    p.add_argument("--no-clip-at-one", action="store_true",
                   help="Do NOT clip normalized performance at 1.0. "
                        "Default is to clip (matches paper figures).")
    p.add_argument("--default-pmq", type=int, default=DEFAULT_PMQ,
                   help=f"PrISM PMQ size for the main CSVs (default {DEFAULT_PMQ}).")
    return p.parse_args()


def main() -> None:
    args = parse_args()

    results_dir = Path(args.results_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Reading from   : {results_dir}")
    print(f"Writing to     : {output_dir}")
    print(f"Cores          : {args.num_cores}")
    print(f"Interface (MT/s): {args.interface}")
    print(f"Clip at 1.0    : {not args.no_clip_at_one}")
    print(f"Default PMQ    : {args.default_pmq}")
    print()

    baseline_df, runs_df = collect_results(results_dir, args.num_cores, args.interface)

    if baseline_df.empty and runs_df.empty:
        print("No results parsed. Exiting.")
        return

    print()
    print(f"Parsed {len(baseline_df)} Baseline runs and {len(runs_df)} non-Baseline runs.")
    if not runs_df.empty:
        print(f"  Per mitigation: {runs_df['mitigation'].value_counts().to_dict()}")
    print()

    # ---- baseline_mpki.csv ----
    if not baseline_df.empty:
        baseline_mpki = baseline_df[["workload", "LLC_MPKI", "RB_MPKI"]].drop_duplicates("workload")
        baseline_mpki.to_csv(output_dir / "baseline_mpki.csv", index=False)
        print(f"Saved: {output_dir / 'baseline_mpki.csv'}")

    # Normalize WS once for all downstream CSVs.
    norm_df = normalize_perf(runs_df, baseline_df,
                             clip_at_one=not args.no_clip_at_one)

    # ---- perf_normalized.csv ----
    perf_df = build_condensed_view(norm_df, value_col="WS_norm",
                                   default_pmq=args.default_pmq)
    perf_df = drop_empty_value_rows(perf_df, MITIGATIONS_NON_BASELINE)
    perf_df = add_summary_rows(perf_df, metric_type="perf",
                               value_cols=MITIGATIONS_NON_BASELINE)
    perf_df = format_and_sort(perf_df, value_cols=MITIGATIONS_NON_BASELINE)
    perf_df = perf_df.rename(columns={"NRH": "TRH-D"})
    perf_df.to_csv(output_dir / "perf_normalized.csv", index=False)
    print(f"Saved: {output_dir / 'perf_normalized.csv'}")

    # ---- rfm_per_trefi.csv ----
    rfm_df = build_condensed_view(norm_df, value_col="RFM_per_tREFI",
                                  default_pmq=args.default_pmq)
    rfm_df = drop_empty_value_rows(rfm_df, MITIGATIONS_NON_BASELINE)
    rfm_df = add_summary_rows(rfm_df, metric_type="rfm",
                              value_cols=MITIGATIONS_NON_BASELINE)
    rfm_df = format_and_sort(rfm_df, value_cols=MITIGATIONS_NON_BASELINE)
    rfm_df = rfm_df.rename(columns={"NRH": "TRH-D"})
    rfm_df.to_csv(output_dir / "rfm_per_trefi.csv", index=False)
    print(f"Saved: {output_dir / 'rfm_per_trefi.csv'}")

    # ---- pmq_sweep.csv ----
    pmq_sweep = build_pmq_sweep(norm_df, nrh=PMQ_SWEEP_NRH, tref=PMQ_SWEEP_TREF)
    if not pmq_sweep.empty:
        pmq_cols = [c for c in pmq_sweep.columns if c.startswith("PMQ")]
        pmq_sweep = add_summary_rows(pmq_sweep, metric_type="perf",
                                     value_cols=pmq_cols)
        pmq_sweep = format_and_sort(pmq_sweep, value_cols=pmq_cols)
        pmq_sweep.to_csv(output_dir / "pmq_sweep.csv", index=False)
        print(f"Saved: {output_dir / 'pmq_sweep.csv'}")


if __name__ == "__main__":
    main()
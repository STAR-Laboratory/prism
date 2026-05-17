"""
PrISM security analysis: worst-case TRH-D under the generalized circular-A-rows
attack, with variable mitigation-window size W.

What this script does
---------------------
1. Computes the analytical worst-case TRH-D for any (W, R, L) configuration
   using the Saroiu-Wolman recurrence.
2. Sweeps the attacker's row-count parameter A over its single-copy regime
   (A >= W) to find the worst-case A* per configuration.
3. Reports the final double-sided threshold:

       TRH_D = floor(T_single / 2) + (QTH + ABO_ACT)

   where T_single is the single-row threshold and (QTH + ABO_ACT) is the
   post-selection / ABO safety margin from the paper's PMQ analysis.

Subcommands
-----------
  figure8   : Fixed-W TRH-D vs L curves with Monte-Carlo validation.
              Produces the CSV consumed by plot_figure5.py.
  sweep-w   : Joint (W, R, L) sweep.
              Produces the CSV consumed by plot_figure6.py.

Default safety margin
---------------------
The PrISM paper uses QTH=4 and ABOACT=12 for the default PMQ size of 16,
giving a +16 TRH-D security margin. Both values are exposed via --qth and
--abo-act if you want to evaluate other PMQ configurations.

ABO-safety constraint
---------------------
All configurations must satisfy W >= 4R (Section III-C of the paper):
under one RFM per Alert, ABO drains one PMQ entry every 4 activations, while
up to R intersections can occur per W-activation window. The sweep utilities
silently filter out configurations that violate this constraint.

Example usage
-------------
# Figure 5 data: fixed W=72, with Monte-Carlo validation
python3 prism_circular_security_analysis.py figure8 --w 72 \
    --output-dir ./results --mc-windows 1000000 --mc-seeds 5

# Figure 6 data: joint W/R/L sweep
python3 prism_circular_security_analysis.py sweep-w \
    --w-values 24,36,48,60,72 --r-min 2 --r-max 18 --l-min 2 --l-max 200 \
    --output-dir ./results
"""

from __future__ import annotations

import argparse
import os
import random
import time
from collections import deque
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass
from math import gcd
from pathlib import Path
from typing import Iterable, Sequence

import pandas as pd


# =============================================================================
# Defaults
# =============================================================================
# PrISM paper baseline: PMQ size 16 gives QTH=4 (tardiness threshold) and
# ABOACT=12 (worst-case activations during chained ABO with a 16-entry PMQ).
DEFAULT_QTH = 4
DEFAULT_ABO_ACT = 12

DEFAULT_W = 72                                      # default mitigation window
NUM_TREFI = 8192                                    # tREFIs per tREFW
PHYS_ACTS_PER_TREFI = 72                            # physical ACT slots per tREFI
TOTAL_SLOTS_PER_TREFW = PHYS_ACTS_PER_TREFI * NUM_TREFI
TARGET_FAIL = 1.01402e-13                           # per-bank failure-rate budget


# =============================================================================
# Data containers
# =============================================================================
@dataclass(frozen=True)
class WorstCaseResult:
    """Analytical worst-case attack point for one (W, R, L) configuration."""
    W: int
    R: int
    L: int
    QTH: int
    ABO_ACT: int
    A_star: int            # worst-case attacker row count
    K_floor_star: int      # floor(L*W / A*)
    X_win_star: float      # A* / W (rows per window)
    N_per_row_star: int    # appearances of any given row in one tREFW
    p_m_star: float        # per-appearance mitigation probability
    TRH_D_base: int        # floor(T_single / 2) before the safety margin
    TRH_D_margin: int      # QTH + ABO_ACT
    TRH_D: int             # final reported threshold


# =============================================================================
# Small helpers
# =============================================================================
def parse_int_list(text: str) -> list[int]:
    """Parse a comma-separated integer list (e.g., '24,36,48,72')."""
    if not text or not text.strip():
        return []
    return [int(x.strip()) for x in text.split(",") if x.strip()]


def validate_security_params(qth: int, abo_act: int) -> None:
    if qth < 0:
        raise ValueError(f"QTH must be non-negative, got {qth}.")
    if abo_act < 0:
        raise ValueError(f"ABO_ACT must be non-negative, got {abo_act}.")


def validate_config(W: int, R: int, L: int,
                    qth: int = DEFAULT_QTH,
                    abo_act: int = DEFAULT_ABO_ACT) -> None:
    validate_security_params(qth, abo_act)
    if W <= 0:
        raise ValueError(f"W must be positive, got {W}.")
    if R <= 0:
        raise ValueError(f"R must be positive, got {R}.")
    if L < 0:
        raise ValueError(f"L must be non-negative, got {L}.")
    if R > W:
        raise ValueError(f"R must be <= W (sampling without replacement); got R={R}, W={W}.")
    # ABO-safety: under one RFM per Alert, ABO drains one pending entry every
    # 4 activations. Since at most R intersections can occur per W-activation
    # window, we need W >= 4R for the SSQ to be bounded. See Section III-C of
    # the PrISM paper.
    if W < 4 * R:
        raise ValueError(
            f"W must satisfy W >= 4R for ABO-safety; got W={W}, R={R} "
            f"(would require W >= {4 * R})."
        )


def trhd_security_margin(qth: int = DEFAULT_QTH,
                         abo_act: int = DEFAULT_ABO_ACT) -> int:
    """Margin added to floor(T_single/2) to yield the final TRH-D.

    QTH (PMQ tardiness threshold) and ABO_ACT (slack activations during chained
    ABO) each contribute single-row activations on the worst-case aggressor;
    their sum is the post-selection margin used in the paper.
    """
    validate_security_params(qth, abo_act)
    return qth + abo_act


def adjusted_trhd_from_single_threshold(T_single: int, qth: int, abo_act: int
                                        ) -> tuple[int, int, int]:
    """Convert a single-row threshold T_single to (base, margin, final TRH-D)."""
    if T_single < 0:
        raise ValueError(f"T_single must be non-negative, got {T_single}.")
    trhd_base = T_single // 2
    margin = trhd_security_margin(qth, abo_act)
    return trhd_base, margin, trhd_base + margin


def security_tag(qth: int, abo_act: int) -> str:
    """Filename tag encoding the safety margin used for a run."""
    return f"QTH{qth}_ABO{abo_act}"


# =============================================================================
# Core analytical primitives
# =============================================================================
def compute_failure_probability(N: int, T: int, p: float, num_target_rows: int) -> float:
    """Saroiu-Wolman cumulative failure probability over N appearances.

    Parameters
    ----------
    N
        Appearances of one target row within one tREFW.
    T
        Single-row threshold (activations).
    p
        Per-appearance mitigation probability.
    num_target_rows
        Number of parallel target rows under the circular-A attack.
    """
    P = [0.0] * (N + 1)
    for k in range(1, N + 1):
        if k < T:
            P[k] = 0.0
        elif k == T:
            P[k] = (1.0 - p) ** T
        else:
            P[k] = p * (1.0 - p) ** T * (1.0 - P[k - T - 1]) + P[k - 1]
    return max(P[1:]) * num_target_rows


def find_min_threshold(N: int, p: float, target_failure_prob: float,
                       num_target_rows: int) -> int:
    """Binary-search the minimum safe single-row threshold T."""
    low, high = 1, max(N, 1)
    best_T = high
    while low <= high:
        mid = (low + high) // 2
        if compute_failure_probability(N, mid, p, num_target_rows) <= target_failure_prob:
            best_T = mid
            high = mid - 1
        else:
            low = mid + 1
    return best_T


def analytical_pm_from_K(W: int, R: int, K: int) -> float:
    """Per-appearance mitigation probability for a row with K prior copies in the SHQ."""
    if K <= 0:
        return 1.0 / W

    # Fixed-point iteration on the per-window "no-intersection" probability.
    P_S = 0.0
    for _ in range(200):
        P_S_new = (K * (R - 1 + P_S ** R)) / (W + K * R)
        if abs(P_S_new - P_S) < 1e-12:
            break
        P_S = P_S_new

    return (1.0 - P_S ** R) / W + (R / W) * P_S


def exact_pm_for_A(W: int, R: int, L: int, A: int) -> float | None:
    """Expected mitigation probability under a circular attack with A rows.

    Requires the single-copy regime A >= W. Computes the exact periodic
    appearance structure of the target row and averages p_m across appearances
    within one period.
    """
    if A < W:
        return None

    g = gcd(W, A)
    period = A // g
    positions = [t for t in range(period) if (-t * W) % A < W]
    if not positions:
        return None

    n = len(positions)
    pm_sum = 0.0

    for i in range(n):
        pos_i = positions[i]
        K = 0
        for j in range(1, L + 2):
            prior_idx = (i - j) % n
            cycles_back = -((i - j) // n) if (i - j) < 0 else 0
            pos_prior = positions[prior_idx]
            windows_back = pos_i - pos_prior + cycles_back * period
            if windows_back <= 0:
                continue
            if windows_back > L:
                break
            K += 1
        pm_sum += analytical_pm_from_K(W, R, K)

    return pm_sum / n


# =============================================================================
# A-sweep and worst-case search
# =============================================================================
def build_A_sweep(W: int, L: int, dense: bool = False) -> list[int]:
    """Single-copy regime sweep: W <= A <= (L+1)*W."""
    stealth_A = (L + 1) * W
    step = 1 if dense else 4
    A_values = sorted(set(
        [W]
        + list(range(W, stealth_A + 1, step))
        + [x * W for x in range(1, L + 2)]
        + [stealth_A]
    ))
    return [a for a in A_values if W <= a <= stealth_A]


def evaluate_A_analytical(
    args: tuple[int, int, int, int, int, int]
) -> tuple[int, int, int, float, int, int, int] | None:
    """Evaluate one A point. Returns
    (A, K_floor, N_per_row, p_m, TRH_D_base, TRH_D_margin, TRH_D) or None.
    """
    W, A, R, L, qth, abo_act = args
    N_per_row = TOTAL_SLOTS_PER_TREFW // max(A, 1)
    if N_per_row < 1:
        return None

    p_m = exact_pm_for_A(W, R, L, A)
    if p_m is None:
        return None

    K_floor = (L * W) // A
    t_single = find_min_threshold(N_per_row, p_m, TARGET_FAIL, A)
    trhd_base, trhd_margin, trhd = adjusted_trhd_from_single_threshold(t_single, qth, abo_act)
    return (A, K_floor, N_per_row, p_m, trhd_base, trhd_margin, trhd)


def find_worst_case_trhd(W: int, R: int, L: int,
                         qth: int = DEFAULT_QTH,
                         abo_act: int = DEFAULT_ABO_ACT,
                         inner_workers: int | None = None) -> WorstCaseResult:
    """Sweep A and return the worst-case (largest) TRH-D for one (W, R, L)."""
    validate_config(W, R, L, qth, abo_act)

    tasks = [(W, A, R, L, qth, abo_act) for A in build_A_sweep(W, L, dense=True)]
    best = None

    def update(candidate):
        nonlocal best
        if candidate is None:
            return
        if best is None or candidate[6] > best[6]:
            best = candidate

    if inner_workers and inner_workers > 1:
        with ProcessPoolExecutor(max_workers=inner_workers) as pool:
            for result in pool.map(evaluate_A_analytical, tasks, chunksize=16):
                update(result)
    else:
        for task in tasks:
            update(evaluate_A_analytical(task))

    if best is None:
        raise RuntimeError(f"No valid A value found for W={W}, R={R}, L={L}.")

    A_star, K_floor_star, N_per_row_star, p_m_star, trhd_base, trhd_margin, trhd = best
    return WorstCaseResult(
        W=W, R=R, L=L, QTH=qth, ABO_ACT=abo_act,
        A_star=A_star, K_floor_star=K_floor_star,
        X_win_star=A_star / W, N_per_row_star=N_per_row_star,
        p_m_star=p_m_star,
        TRH_D_base=trhd_base, TRH_D_margin=trhd_margin, TRH_D=trhd,
    )


# =============================================================================
# Batch evaluation
# =============================================================================
def _evaluate_config_worker(args):
    W, R, L, qth, abo_act = args
    worst = find_worst_case_trhd(W, R, L, qth=qth, abo_act=abo_act)
    return {
        "W": W, "R": R, "L": L,
        "QTH": qth, "ABO_ACT": abo_act,
        "single_row_security_margin": qth + abo_act,
        "TRH_D_margin": worst.TRH_D_margin,
        "SHQ_entries": (R - 1) * L,
        "sampling_fraction": R / W,
        "default_mitigation_rate": 1 / W,
        "TRH_D_base": worst.TRH_D_base,
        "TRH_D": worst.TRH_D,
        "A_star": worst.A_star,
        "K_floor_star": worst.K_floor_star,
        "X_win_star": worst.X_win_star,
        "N_per_row_star": worst.N_per_row_star,
        "p_m_star": worst.p_m_star,
    }


def run_batch(configs: Sequence[tuple[int, int, int]],
              qth: int = DEFAULT_QTH,
              abo_act: int = DEFAULT_ABO_ACT,
              outer_workers: int | None = None,
              progress: bool = True) -> pd.DataFrame:
    """Evaluate many (W, R, L) configurations in parallel."""
    validate_security_params(qth, abo_act)
    if outer_workers is None:
        outer_workers = os.cpu_count() or 1

    # Filter invalid / ABO-unsafe configs rather than crashing the whole sweep.
    valid = [(W, R, L, qth, abo_act) for (W, R, L) in configs
             if W > 0 and R > 0 and L >= 0 and R <= W and W >= 4 * R]

    results = []
    start = time.time()

    if outer_workers > 1 and len(valid) > 1:
        with ProcessPoolExecutor(max_workers=outer_workers) as pool:
            futures = {pool.submit(_evaluate_config_worker, cfg): cfg for cfg in valid}
            for i, future in enumerate(as_completed(futures), 1):
                r = future.result()
                results.append(r)
                if progress:
                    print(f"[{i}/{len(valid)}] W={r['W']}, R={r['R']}, L={r['L']}: "
                          f"TRH-D={r['TRH_D']} (base={r['TRH_D_base']}, +{r['TRH_D_margin']}) "
                          f"@ A*={r['A_star']}")
    else:
        for i, cfg in enumerate(valid, 1):
            r = _evaluate_config_worker(cfg)
            results.append(r)
            if progress:
                print(f"[{i}/{len(valid)}] W={r['W']}, R={r['R']}, L={r['L']}: "
                      f"TRH-D={r['TRH_D']} (base={r['TRH_D_base']}, +{r['TRH_D_margin']}) "
                      f"@ A*={r['A_star']}")

    df = pd.DataFrame(results)
    if not df.empty:
        df = df.sort_values(["W", "R", "L"]).reset_index(drop=True)

    if progress:
        print(f"Completed {len(valid)} configurations in {time.time() - start:.1f}s")
    return df


def get_broader_sweep_configs(w_values: Iterable[int] = (DEFAULT_W,),
                              r_values: Iterable[int] = range(2, 9),
                              l_values: Iterable[int] = range(2, 40),
                              ) -> list[tuple[int, int, int]]:
    """Cartesian product of (W, R, L), keeping only ABO-safe configurations
    (R <= W and W >= 4R; see Section III-C of the PrISM paper).
    """
    return [(W, R, L)
            for W in w_values
            for L in l_values
            for R in r_values
            if R <= W and W >= 4 * R]


# =============================================================================
# Monte-Carlo validation (used by the figure8 subcommand)
# =============================================================================
def _mc_single_seed(W: int, R: int, L: int, A: int,
                    num_windows: int, seed: int) -> tuple[int, int]:
    """One seed of the circular-A Monte-Carlo simulation.
    Returns (target_appearances, target_mitigations)."""
    rng = random.Random(seed)
    target = 0
    shq: deque[tuple[int, int]] = deque()
    target_appearances = 0
    target_mitigations = 0

    # Burn-in lets the SHQ reach steady-state before measurement starts.
    warmup = 10 * L + (A // W + 1) * 10

    for t in range(num_windows + warmup):
        sampled_slots = rng.sample(range(W), R)
        candidate_rows = [(t * W + s) % A for s in sampled_slots]

        shq_set = {row for row, _ in shq}
        intersections = [r for r in candidate_rows if r in shq_set]
        non_intersections = [r for r in candidate_rows if r not in shq_set]

        mitigated = set()
        if non_intersections:
            mitigated.add(rng.choice(non_intersections))
        else:
            mitigated.add(rng.choice(candidate_rows))
        mitigated.update(intersections)

        if t >= warmup:
            target_slots = [s for s in range(W) if (t * W + s) % A == target]
            if target_slots:
                target_appearances += len(target_slots)
                if target in mitigated:
                    target_mitigations += len(target_slots)

        for row in candidate_rows:
            if row not in mitigated:
                shq.append((row, t + L))
        while shq and shq[0][1] <= t:
            shq.popleft()

    return target_appearances, target_mitigations


def mc_measure_pm(W: int, R: int, L: int, A: int,
                  num_windows: int = 500_000,
                  num_seeds: int = 5,
                  base_seed: int = 42) -> tuple[float, int]:
    """Aggregate p_m estimate across multiple seeds."""
    validate_config(W, R, L)
    if A < W:
        raise ValueError(f"Monte-Carlo assumes A >= W; got A={A}, W={W}.")

    total_app = total_mit = 0
    for offset in range(num_seeds):
        app, mit = _mc_single_seed(W, R, L, A, num_windows, base_seed + offset)
        total_app += app
        total_mit += mit
    return (total_mit / total_app if total_app else 0.0), total_app


def evaluate_rl_with_mc(args) -> dict:
    """One (W, R, L) point for the figure8 plot, with optional MC validation."""
    W, R, L, qth, abo_act, run_mc, mc_windows, mc_seeds = args
    worst = find_worst_case_trhd(W, R, L, qth=qth, abo_act=abo_act)

    row = {
        "W": W, "R": R, "L": L,
        "QTH": qth, "ABO_ACT": abo_act,
        "single_row_security_margin": qth + abo_act,
        "TRH_D_margin": worst.TRH_D_margin,
        "SHQ_entries": (R - 1) * L,
        "sampling_fraction": R / W,
        "A_star": worst.A_star,
        "K_floor_star": worst.K_floor_star,
        "X_win_star": worst.X_win_star,
        "N_per_row_star": worst.N_per_row_star,
        "p_m_analytical": worst.p_m_star,
        "TRH_D_base_analytical": worst.TRH_D_base,
        "TRH_D_analytical": worst.TRH_D,
    }

    if run_mc:
        sim_pm, n_app = mc_measure_pm(W, R, L, worst.A_star,
                                      num_windows=mc_windows, num_seeds=mc_seeds)
        t_sim = find_min_threshold(worst.N_per_row_star, sim_pm,
                                   TARGET_FAIL, worst.A_star)
        base_sim, margin, trhd_sim = adjusted_trhd_from_single_threshold(t_sim, qth, abo_act)
        row.update({
            "p_m_simulated": sim_pm,
            "TRH_D_base_simulated": base_sim,
            "TRH_D_margin_simulated": margin,
            "TRH_D_simulated": trhd_sim,
            "mc_target_appearances": n_app,
            "mc_windows": mc_windows,
            "mc_seeds": mc_seeds,
        })
    return row


def generate_figure8_data(W: int,
                          l_values: Sequence[int],
                          r_values: Sequence[int],
                          qth: int = DEFAULT_QTH,
                          abo_act: int = DEFAULT_ABO_ACT,
                          sim_l_values: Sequence[int] | None = None,
                          mc_windows: int = 500_000,
                          mc_seeds: int = 5,
                          outer_workers: int | None = None) -> pd.DataFrame:
    """Compute the analytical + Monte-Carlo (W, R, L) grid for plot_figure5.py."""
    validate_security_params(qth, abo_act)
    if sim_l_values is None:
        sim_l_values = list(l_values)
    if outer_workers is None:
        outer_workers = os.cpu_count() or 1

    # Skip any R that violates R <= W or the ABO-safety constraint W >= 4R.
    tasks = [(W, R, L, qth, abo_act, L in sim_l_values, mc_windows, mc_seeds)
             for R in r_values if R <= W and W >= 4 * R
             for L in l_values]

    results = []
    start = time.time()
    with ProcessPoolExecutor(max_workers=outer_workers) as pool:
        futures = {pool.submit(evaluate_rl_with_mc, t): t for t in tasks}
        for i, fut in enumerate(as_completed(futures), 1):
            results.append(fut.result())
            if i % 10 == 0 or i == len(tasks):
                print(f"[{i}/{len(tasks)}] figure points completed")
    print(f"Figure evaluation finished in {time.time() - start:.1f}s")

    return pd.DataFrame(results).sort_values(["R", "L"]).reset_index(drop=True)


# =============================================================================
# Command-line entry points
# =============================================================================
def command_sweep_w(args):
    w_values = parse_int_list(args.w_values) or list(range(args.w_min, args.w_max + 1, args.w_step))
    configs = get_broader_sweep_configs(
        w_values=w_values,
        r_values=range(args.r_min, args.r_max + 1),
        l_values=range(args.l_min, args.l_max + 1),
    )
    df = run_batch(configs, qth=args.qth, abo_act=args.abo_act,
                   outer_workers=args.workers, progress=True)
    out_csv = Path(args.output_dir) / f"sweep_W_R_L_{security_tag(args.qth, args.abo_act)}.csv"
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(out_csv, index=False)
    print(f"\nSaved: {out_csv}")


def command_figure8(args):
    l_values = list(range(args.l_min, args.l_max + 1))
    r_values = list(range(args.r_min, args.r_max + 1))
    sim_l_values = (
        list(range(args.sim_l_min, args.sim_l_max + 1))
        if args.sim_l_min is not None and args.sim_l_max is not None
        else l_values
    )

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    tag = security_tag(args.qth, args.abo_act)
    csv_path = output_dir / f"figure8_generalized_with_mc_W{args.w}_{tag}.csv"

    df = generate_figure8_data(
        W=args.w, l_values=l_values, r_values=r_values,
        qth=args.qth, abo_act=args.abo_act,
        sim_l_values=sim_l_values,
        mc_windows=args.mc_windows, mc_seeds=args.mc_seeds,
        outer_workers=args.workers,
    )
    df.to_csv(csv_path, index=False)
    print(f"\nSaved: {csv_path}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="PrISM circular-A security analysis (variable W).")
    subparsers = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    common.add_argument("--output-dir", type=str, default="./results")

    sec = argparse.ArgumentParser(add_help=False)
    sec.add_argument("--qth", type=int, default=DEFAULT_QTH,
                     help="PMQ tardiness threshold contribution (default 4)")
    sec.add_argument("--abo-act", type=int, default=DEFAULT_ABO_ACT,
                     help="ABO slack-activation contribution (default 12 for PMQ=16)")

    var_w = argparse.ArgumentParser(add_help=False)
    var_w.add_argument("--w-values", type=str, default="",
                       help="Comma-separated W values, e.g., 24,36,48,72")
    var_w.add_argument("--w-min", type=int, default=24)
    var_w.add_argument("--w-max", type=int, default=72)
    var_w.add_argument("--w-step", type=int, default=12)
    var_w.add_argument("--r-min", type=int, default=2)
    var_w.add_argument("--r-max", type=int, default=18)
    var_w.add_argument("--l-min", type=int, default=2)
    var_w.add_argument("--l-max", type=int, default=39)

    p = subparsers.add_parser("sweep-w", parents=[common, sec, var_w],
                              help="Joint W/R/L sweep (CSV for plot_figure6.py)")
    p.set_defaults(func=command_sweep_w)

    p = subparsers.add_parser("figure8", parents=[common, sec],
                              help="Fixed-W TRH-D vs L curves with MC validation "
                                   "(CSV for plot_figure5.py)")
    p.add_argument("--w", type=int, default=DEFAULT_W)
    p.add_argument("--mc-windows", type=int, default=1_000_000)
    p.add_argument("--mc-seeds", type=int, default=5)
    p.add_argument("--l-min", type=int, default=5)
    p.add_argument("--l-max", type=int, default=25)
    p.add_argument("--r-min", type=int, default=2)
    p.add_argument("--r-max", type=int, default=18)
    p.add_argument("--sim-l-min", type=int, default=None)
    p.add_argument("--sim-l-max", type=int, default=None)
    p.set_defaults(func=command_figure8)

    return parser


def main() -> None:
    args = build_parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
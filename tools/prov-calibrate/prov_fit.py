"""prov_fit: fit the provenance-size estimate of Odin III (src/passes/prov_depth_fit.h).

    prov_fit.py TABLE.tsv [--header OUT.h] [--max-terms N]

TABLE.tsv is the calibration table (docs/specs/2026-11-13-prov-levels-calibration.tsv): one row per
design and depth, with the features of the design after ``proc`` and the sizes written. For each
depth and measure (records, edges, name-map bytes, step-file bytes) the model is a power law,
``value = exp(scale + sum(exponent_f * ln(1 + feature_f)))``, with at most ``--max-terms`` features
(default 3) chosen by the smallest worst ratio, so that the largest over- or under-estimate is as
small as the data allow. Prints the fit, the worst ratio and a leave-one-out worst ratio, and with
``--header`` writes the C table. Standard library only.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import math
import sys
from pathlib import Path

FEATURES = ("cells", "pins", "outs", "regs", "arith", "logic", "mux", "shift", "mul", "cons")
MEASURES = ("records", "edges", "map_bytes", "step_bytes")
DEPTHS = (0, 1, 2, 3, 4)
SLOTS = {"records": "METRIC_RECORDS", "edges": "METRIC_EDGES", "map_bytes": "METRIC_MAP",
         "step_bytes": "METRIC_STEPS"}
REFINE_ROUNDS = 200


Table = dict[tuple[int, str], tuple[tuple[int, ...], list[float]]]


class Row:
    def __init__(self, record: dict[str, str]) -> None:
        self.design = record["design"]
        self.depth = int(record["depth"])
        self.x = [math.log1p(float(record[name])) for name in FEATURES]
        self.y = {name: float(record[name]) for name in MEASURES}


def solve(matrix: list[list[float]], rhs: list[float]) -> list[float] | None:
    """Gauss-Jordan with partial pivoting; None when the system is singular."""
    size = len(rhs)
    aug = [row[:] + [rhs[i]] for i, row in enumerate(matrix)]
    for col in range(size):
        pivot = max(range(col, size), key=lambda r: abs(aug[r][col]))
        if abs(aug[pivot][col]) < 1e-12:
            return None
        aug[col], aug[pivot] = aug[pivot], aug[col]
        for r in range(size):
            if r != col:
                factor = aug[r][col] / aug[col][col]
                aug[r] = [a - factor * b for a, b in zip(aug[r], aug[col], strict=True)]
    return [aug[i][size] / aug[i][i] for i in range(size)]


def least_squares(rows: list[list[float]], target: list[float],
                  weight: list[float]) -> list[float] | None:
    width = len(rows[0])
    ata = [[sum(w * r[i] * r[j] for r, w in zip(rows, weight, strict=True))
            for j in range(width)] for i in range(width)]
    atb = [sum(w * r[i] * t for r, t, w in zip(rows, target, weight, strict=True))
           for i in range(width)]
    return solve(ata, atb)


def design_row(row: Row, subset: tuple[int, ...]) -> list[float]:
    return [1.0] + [row.x[i] for i in subset]


def worst_ratio(coef: list[float], rows: list[Row], subset: tuple[int, ...], measure: str) -> float:
    worst = 1.0
    for row in rows:
        pred = sum(c * v for c, v in zip(coef, design_row(row, subset), strict=True))
        err = abs(pred - math.log(max(row.y[measure], 1.0)))
        worst = max(worst, math.exp(err))
    return worst


def fit_subset(rows: list[Row], subset: tuple[int, ...], measure: str) -> list[float] | None:
    """Least squares in log space, then a few reweighting rounds that pull the worst row in.
    None when an exponent comes out negative: a bigger design never writes less."""
    mat = [design_row(r, subset) for r in rows]
    target = [math.log(max(r.y[measure], 1.0)) for r in rows]
    weight = [1.0] * len(rows)
    best: list[float] | None = None
    best_worst = math.inf
    for _ in range(REFINE_ROUNDS):
        coef = least_squares(mat, target, weight)
        if coef is None or any(c < 0.0 for c in coef[1:]):
            return best
        res = [abs(sum(c * v for c, v in zip(coef, m, strict=True)) - t)
               for m, t in zip(mat, target, strict=True)]
        worst = math.exp(max(res))
        if worst < best_worst:
            best, best_worst = coef, worst
        top = max(res) or 1.0
        weight = [w * (1.0 + res_i / top) for w, res_i in zip(weight, res, strict=True)]
        norm = sum(weight) / len(weight)
        weight = [w / norm for w in weight]
    return best


def best_fit(rows: list[Row], measure: str,
             max_terms: int) -> tuple[float, tuple[int, ...], list[float]] | None:
    best: tuple[float, tuple[int, ...], list[float]] | None = None
    for count in range(0, max_terms + 1):
        for subset in itertools.combinations(range(len(FEATURES)), count):
            if len(rows) <= count + 1:
                continue
            coef = fit_subset(rows, subset, measure)
            if coef is None:
                continue
            worst = worst_ratio(coef, rows, subset, measure)
            if best is None or worst < best[0] - 1e-9:
                best = (worst, subset, coef)
    return best


def leave_one_out(rows: list[Row], subset: tuple[int, ...], measure: str) -> float:
    worst = 1.0
    for i, row in enumerate(rows):
        rest = rows[:i] + rows[i + 1:]
        coef = fit_subset(rest, subset, measure)
        if coef is None:
            return math.inf
        pred = sum(c * v for c, v in zip(coef, design_row(row, subset), strict=True))
        worst = max(worst, math.exp(abs(pred - math.log(max(row.y[measure], 1.0)))))
    return worst


def c_row(subset: tuple[int, ...], coef: list[float]) -> str:
    exps = [0.0] * len(FEATURES)
    for index, value in zip(subset, coef[1:], strict=True):
        exps[index] = value
    inner = ", ".join(f"{value:.6g}" for value in exps)
    return f"{{true, {coef[0]:.6g}, {{{inner}}}}}"


def header(table: Table) -> str:
    lines = ["/* Written by tools/prov-calibrate/prov_fit.py from the calibration table. */",
             "static const fit_row k_fit[ODIN3_PROV_DEPTH_COUNT][METRIC_COUNT] = {"]
    unused = "{false, 0.0, {0.0}}"
    for depth in DEPTHS:
        cells = []
        for measure in MEASURES:
            entry = table.get((depth, measure))
            cells.append(f"[{SLOTS[measure]}] = {c_row(*entry)}" if entry else
                         f"[{SLOTS[measure]}] = {unused}")
        lines.append(f"    [{depth}] = {{{', '.join(cells)}}},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def predict(row: Row, entry: tuple[tuple[int, ...], list[float]]) -> float:
    subset, coef = entry
    return math.exp(sum(c * v for c, v in zip(coef, design_row(row, subset), strict=True)))


def markdown(rows: list[Row], table: Table) -> str:
    """One line per design: the real name-map bytes at depths 1 to 4 and the estimate/real ratio
    of every measure; the last column is the step files at depth 4."""
    out = ["| design | map1 real | est/real d1 | d2 | d3 | d4 | steps4 real | est/real |",
           "|---|---|---|---|---|---|---|---|"]
    by_design: dict[str, dict[int, Row]] = {}
    for row in rows:
        by_design.setdefault(row.design, {})[row.depth] = row
    for design, depths in by_design.items():
        cells = [design, f"{depths[1].y['map_bytes']:.0f}" if 1 in depths else "-"]
        floor = 0.0  # depths 2 to 4 never drop below the depth before (src/passes/prov_depth.c)
        for depth in (1, 2, 3, 4):
            entry = table.get((depth, "map_bytes"))
            if entry and depth in depths:
                row = depths[depth]
                guess = predict(row, entry)
                guess = max(guess, floor) if depth >= 3 else guess
                floor = guess if depth >= 2 else floor
                cells.append(f"{guess / max(row.y['map_bytes'], 1.0):.2f}")
            else:
                cells.append("-")
        entry = table.get((4, "step_bytes"))
        real = depths[4].y["step_bytes"] if 4 in depths else 0.0
        cells.append(f"{real:.0f}")
        cells.append(f"{predict(depths[4], entry) / max(real, 1.0):.2f}" if entry and 4 in depths
                     else "-")
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out) + "\n"


def load(path: Path) -> list[Row]:
    with path.open(newline="", encoding="utf-8") as handle:
        return [Row(rec) for rec in csv.DictReader(handle, delimiter="\t")]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", maxsplit=1)[0])
    ap.add_argument("table", type=Path)
    ap.add_argument("--header", type=Path)
    ap.add_argument("--markdown", type=Path)
    ap.add_argument("--max-terms", type=int, default=3)
    ns = ap.parse_args(argv)
    rows = load(ns.table)
    table: Table = {}
    for depth in (1, 2, 3, 4):
        for measure in MEASURES:
            if measure == "step_bytes" and depth != 4:
                continue
            sub = [r for r in rows if r.depth == depth]
            found = best_fit(sub, measure, ns.max_terms)
            if found is None:
                print(f"depth {depth} {measure}: no fit", file=sys.stderr)
                continue
            worst, subset, coef = found
            loo = leave_one_out(sub, subset, measure)
            terms = zip(subset, coef[1:], strict=True)
            names = ", ".join(f"{FEATURES[i]}^{c:.3f}" for i, c in terms)
            print(f"depth {depth} {measure:10s} worst x{worst:.2f} leave-one-out x{loo:.2f}  "
                  f"exp({coef[0]:.3f}) * {names or '1'}")
            table[(depth, measure)] = (subset, coef)
    if ns.header:
        ns.header.write_text(header(table), encoding="utf-8")
    if ns.markdown:
        ns.markdown.write_text(markdown(rows, table), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

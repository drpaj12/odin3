"""timing_hints: back-annotate a routed pass-1 run into a timing hint file (docs/specs/
2026-11-14-timing-hints.md, docs/user/timing-hints.md).

Input: the mapped BLIF of pass 1, its version 2 name map written with ``ODIN3_PROV_UID=1`` (the
``uid=`` flag is the hint key) and VPR's ``report_net_timing.csv`` of one or more seeds. Output: a
hint file for ``read_hints`` (plain text, TAB separated).

Method (greedy, "assume the worst"): the slack of a LUT is the smallest slack over all its pins in
the routed net timing report. A LUT whose provenance record has an exact cut gives its slack to each
parent record (the gate records of its cut, whose ``uid`` is the hint key); each record keeps the
smallest slack of its LUTs. With several seeds the slack of a LUT is relative to the worst slack of
its own seed, and the smallest over the seeds is kept. Ties (between pins of one LUT, between
LUTs of one record) are broken at random with a fixed recorded seed; the choice only names the
witness (``--witness``), never the hint values. Standard library only.
"""

from __future__ import annotations

import argparse
import csv
import random
import re
import sys
from collections import defaultdict
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path

import name_map_v2

MAGIC = "odin3-timing-hints"
VERSION = 1
PS_PER_NS = 1000.0
DEFAULT_TIE_SEED = 20261114
DEFAULT_MAX_CELLS = 8
OUT_PORT = re.compile(r"^(out\d*|sumout|cout|dataout\d*|q)(\[\d+\])?$")


class HintError(Exception):
    """An input the back-annotation cannot use."""


@dataclass
class Blif:
    """What the unit-depth estimate needs of a mapped BLIF."""

    luts: dict[str, list[str]] = field(default_factory=dict)  # LUT output -> input nets
    depth: dict[str, int] = field(default_factory=dict)  # net -> unit depth
    max_depth: int = 0


def read_blif(path: Path) -> Blif:
    """LUTs (a .names with two or more inputs) and the unit depth of every net: a LUT costs 1, a
    one-input or constant .names 0; flops, hard blocks and inputs start a path (as ABC sees it)."""
    text = path.read_text(errors="replace").replace("\\\n", " ")
    gates: dict[str, list[str]] = {}
    sources: set[str] = set()
    for line in text.splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == ".names":
            gates[words[-1]] = words[1:-1]
        elif words[0] == ".latch":
            sources.add(words[2])
        elif words[0] in (".inputs",):
            sources.update(words[1:])
        elif words[0] == ".subckt":
            for pin in words[2:]:
                port, _, net = pin.partition("=")
                if net and OUT_PORT.match(port):
                    sources.add(net)
    blif = Blif()
    blif.luts = {out: ins for out, ins in gates.items() if len(ins) >= 2}
    depth = blif.depth
    for out in gates:
        _depth_of(out, gates, sources, depth)
    blif.max_depth = max((depth[o] for o in blif.luts), default=0)
    return blif


def _depth_of(net: str, gates: Mapping[str, list[str]], sources: set[str],
              depth: dict[str, int]) -> int:
    """Unit depth of net, iterative (no recursion: designs are deep)."""
    stack = [net]
    while stack:
        cur = stack[-1]
        if cur in depth:
            stack.pop()
            continue
        if cur in sources or cur not in gates:
            depth[cur] = 0
            stack.pop()
            continue
        pending = [x for x in gates[cur] if x not in depth]
        if pending:
            stack.extend(pending)
            continue
        depth[cur] = (1 if len(gates[cur]) >= 2 else 0) + max(
            (depth[x] for x in gates[cur]), default=0)
        stack.pop()
    return depth[net]


@dataclass
class SeedSlack:
    """The routed slack of every atom of one VPR run, in ps, relative to the run's worst slack."""

    cpd_ns: float
    worst_ps: float
    rel_ps: dict[str, float]  # atom -> slack - worst slack, ps (>= 0)
    pin_ps: dict[str, list[tuple[float, str]]]  # atom -> (slack ps, pin) of its smallest pins


def read_net_timing(path: Path, cpd_ns: float) -> SeedSlack:
    """The worst slack per atom of report_net_timing.csv (source pin and sink pins of every net)."""
    csv.field_size_limit(1 << 30)
    with path.open(newline="") as fh:
        names = csv.DictReader(fh).fieldnames or []
    missing = [c for c in ("src_pin_name", "src_pin_slack", "sinks") if c not in names]
    if missing:
        raise HintError(f"{path}: no column {', '.join(missing)} (is it VPR's "
                        f"report_net_timing.csv?)")
    best: dict[str, float] = {}
    pins: dict[str, list[tuple[float, str]]] = {}

    def upd(pin: str, slack: str) -> None:
        try:
            value = float(slack) * 1e12  # s -> ps
        except ValueError:
            return
        if value != value or value in (float("inf"), float("-inf")):
            return
        atom = pin.rsplit(".", 1)[0]
        cur = best.get(atom)
        if cur is None or value < cur:
            best[atom] = value
            pins[atom] = [(value, pin)]
        elif value == cur:
            pins[atom].append((value, pin))

    with path.open(newline="") as fh:
        for row in csv.DictReader(fh):
            upd(row["src_pin_name"], row["src_pin_slack"])
            for sink in row["sinks"].split(";"):
                if sink:
                    pin, slack, _ = sink.rsplit(",", 2)
                    upd(pin, slack)
    if not best:
        raise HintError(f"{path}: no pin slack found (is it VPR's report_net_timing.csv?)")
    worst = min(best.values())
    return SeedSlack(cpd_ns, worst, {a: v - worst for a, v in best.items()}, pins)


@dataclass
class Hint:
    uid: int
    slack_ps: float
    crit: float
    depth: int
    luts: int
    witness: str = ""


@dataclass
class Stats:
    luts: int = 0
    luts_with_slack: int = 0
    luts_exact: int = 0
    luts_approx: int = 0
    luts_no_cut: int = 0
    luts_skipped_wide: int = 0
    luts_hinted: int = 0
    cells: int = 0
    no_uid: int = 0


def lut_parents(nm: name_map_v2.NameMap, atom: str, cut_mode: str,
                max_cells: int) -> tuple[str, list[int]]:
    """(cut class, parent record numbers that carry a hint key) of one LUT atom."""
    obj = nm.atom(atom)
    if obj is None or not obj.records:
        return "none", []
    rec = nm.records[obj.records[0] - 1]
    flags = dict(f.partition("=")[::2] for f in rec.flags)
    cut = flags.get("cut", "none")
    parents: list[int] = []
    if cut == "exact":
        parents = list(rec.parents)
    elif cut == "approx" and cut_mode == "all" and rec.parents:
        region = nm.records[rec.parents[0] - 1]
        parents = list(region.parents) or [rec.parents[0]]
    if max_cells and len(parents) > max_cells:
        return "wide", []
    return cut, parents


def uid_of(nm: name_map_v2.NameMap, number: int) -> int | None:
    if not 1 <= number <= len(nm.records):
        return None
    for flag in nm.records[number - 1].flags:
        if flag.startswith("uid="):
            return int(flag[4:])
    return None


def annotate(blif: Blif, nm: name_map_v2.NameMap, seeds: Sequence[SeedSlack], cut_mode: str,
             max_cells: int, tie_seed: int) -> tuple[list[Hint], Stats]:
    """The hints of one design: per record the smallest relative slack of its LUTs."""
    rng = random.Random(tie_seed)
    stats = Stats(luts=len(blif.luts))
    groups: dict[int, list[tuple[float, float, int, str]]] = defaultdict(list)
    for atom in sorted(blif.luts):
        rels = [(s.rel_ps[atom], s) for s in seeds if atom in s.rel_ps]
        if not rels:
            continue
        stats.luts_with_slack += 1
        slack, seed = min(rels, key=lambda r: r[0])
        crit = 1.0 - slack / (seed.cpd_ns * PS_PER_NS) if seed.cpd_ns > 0 else 1.0
        tied = [pin for value, pin in seed.pin_ps.get(atom, [])
                if value - seed.worst_ps == slack] or [atom]
        witness = rng.choice(sorted(tied))
        cut, parents = lut_parents(nm, atom, cut_mode, max_cells)
        stats.luts_exact += cut == "exact"
        stats.luts_approx += cut == "approx"
        stats.luts_no_cut += cut in ("none", "")
        stats.luts_skipped_wide += cut == "wide"
        used = False
        for number in parents:
            uid = uid_of(nm, number)
            if uid is None:
                stats.no_uid += 1
                continue
            groups[uid].append((slack, min(1.0, max(0.0, crit)), blif.depth[atom], witness))
            used = True
        stats.luts_hinted += used
    hints = []
    for uid in sorted(groups):
        rows = groups[uid]
        low = min(r[0] for r in rows)
        pick = rng.choice(sorted(r[3] for r in rows if r[0] == low))
        hints.append(Hint(uid, low, max(r[1] for r in rows), min(r[2] for r in rows), len(rows),
                          pick))
    stats.cells = len(hints)
    return hints, stats


def hint_text(hints: Iterable[Hint], meta: Mapping[str, str]) -> str:
    lines = [f"{MAGIC}\t{VERSION}"]
    lines += [f"meta\t{k}\t{v}" for k, v in meta.items()]
    lines += [f"cell\t{h.uid}\t{h.slack_ps:.1f}\t{h.crit:.4f}\t{h.depth}\t{h.luts}" for h in hints]
    return "\n".join(lines) + "\n"


RESERVED_META = ("cpd_ps", "depth", "slack_min_ps", "ns_per_level")  # the numbers pass 1 measured


def check_meta(extra: Mapping[str, str]) -> None:
    """HintError for a meta key or value with a control character (it would add a record or a
    field to the file), an empty key, or a key that overrides a measured number."""
    for key, value in extra.items():
        if not key or any(ord(c) < 32 or ord(c) == 127 for c in key + value):
            raise HintError(f"--meta {key!r}={value!r}: a key must not be empty and neither "
                            f"part may hold a control character (tab, newline, ...)")
        if key in RESERVED_META:
            raise HintError(f"--meta {key}: this key is measured from pass 1 and cannot be set")


def build(blif_path: Path, map_path: Path, csvs: Sequence[Path], cpds: Sequence[float],
          cut_mode: str = "exact", max_cells: int = DEFAULT_MAX_CELLS,
          tie_seed: int = DEFAULT_TIE_SEED,
          extra_meta: Mapping[str, str] | None = None) -> tuple[str, Stats, list[Hint]]:
    """The hint file text for pass 1's BLIF, name map and routed net timing reports."""
    check_meta(extra_meta or {})
    if len(csvs) != len(cpds) or not csvs:
        raise HintError("one --cpd-ns per net timing report is needed")
    blif = read_blif(blif_path)
    nm = name_map_v2.read_name_map(map_path)
    seeds = [read_net_timing(p, c) for p, c in zip(csvs, cpds, strict=True)]
    hints, stats = annotate(blif, nm, seeds, cut_mode, max_cells, tie_seed)
    if blif.max_depth == 0:
        raise HintError(f"{blif_path}: no LUT with two or more inputs")
    if stats.cells == 0:
        raise HintError(f"{map_path}: no LUT record gave a hint key (needs ODIN3_PROV_UID=1 and "
                        f"an exact cut, abc --prov-cut on)")
    cpd_ns = sum(cpds) / len(cpds)
    meta: dict[str, str] = {
        "cpd_ps": f"{cpd_ns * PS_PER_NS:.1f}",
        "slack_min_ps": "0",
        "depth": str(blif.max_depth),
        "pass": "1",
        "seeds": str(len(csvs)),
        "reduce": "worst" if len(csvs) > 1 else "single",
        "cut": cut_mode,
        "max_cells": str(max_cells),
        "tie_seed": str(tie_seed),
    }
    meta.update(extra_meta or {})
    return hint_text(hints, meta), stats, hints


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="timing_hints",
                                 description="write a timing hint file from a routed pass-1 run")
    ap.add_argument("--blif", type=Path, required=True, help="pass 1's mapped BLIF")
    ap.add_argument("--map", type=Path, required=True,
                    help="its version 2 name map (ODIN3_PROV_UID=1, exact cuts)")
    ap.add_argument("--net-timing", type=Path, action="append", required=True,
                    help="VPR report_net_timing.csv (repeat per seed)")
    ap.add_argument("--cpd-ns", type=float, action="append", required=True,
                    help="routed CPD of that seed in ns (repeat, same order)")
    ap.add_argument("--out", type=Path, required=True, help="the hint file to write")
    ap.add_argument("--cut", choices=("exact", "all"), default="exact")
    ap.add_argument("--max-cells", type=int, default=DEFAULT_MAX_CELLS,
                    help="skip a LUT whose cut covers more cells (0: no limit; default 8)")
    ap.add_argument("--tie-seed", type=int, default=DEFAULT_TIE_SEED)
    ap.add_argument("--meta", action="append", default=[], metavar="KEY=VALUE",
                    help="extra meta line, e.g. arch=..., vpr=... (repeatable)")
    ap.add_argument("--witness", type=Path, help="write the witness pin of each hint (TSV)")
    return ap.parse_args(list(argv))


def main(argv: Sequence[str] | None = None) -> int:
    ns = parse_args(sys.argv[1:] if argv is None else argv)
    extra = dict(item.partition("=")[::2] for item in ns.meta)
    try:
        text, stats, hints = build(ns.blif, ns.map, ns.net_timing, ns.cpd_ns, ns.cut,
                                   ns.max_cells, ns.tie_seed, extra)
    except (HintError, OSError, ValueError) as err:
        print(f"timing_hints: error: {err}", file=sys.stderr)
        return 1
    ns.out.write_text(text)
    if ns.witness is not None:
        ns.witness.write_text("".join(f"{h.uid}\t{h.witness}\n" for h in hints))
    print(f"timing_hints: {stats.cells} cell hints from {stats.luts_hinted} of {stats.luts} LUTs "
          f"(exact cut {stats.luts_exact}, approx {stats.luts_approx}, none {stats.luts_no_cut}, "
          f"wide {stats.luts_skipped_wide}); written to {ns.out.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

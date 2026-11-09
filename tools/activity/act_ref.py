"""Independent reference of the activity pass (spec 2026-11-08-activity-design.md, section 3.1).

Reads a combinational BLIF (``.names`` with ON-set rows) and enumerates every pair of input
assignments (old frame, new frame) with the two-frame joint of each input: no code shared with the
pass. ``reference(blif_text, pi_prob, pi_density)`` returns {net: (p1, density)}.
Standard library only.
"""

from __future__ import annotations

import itertools


def parse(text: str) -> tuple[list[str], list[tuple[list[str], str, list[str]]]]:
    inputs: list[str] = []
    nodes: list[tuple[list[str], str, list[str]]] = []
    cur: tuple[list[str], str, list[str]] | None = None
    for raw in text.splitlines():
        line = raw.split("#")[0].strip()
        if not line:
            continue
        tok = line.split()
        if tok[0] == ".inputs":
            inputs += tok[1:]
        elif tok[0] == ".names":
            cur = (tok[1:-1], tok[-1], [])
            nodes.append(cur)
        elif not tok[0].startswith(".") and cur is not None:
            cur[2].append(line)
        else:
            cur = None
    return inputs, nodes


def eval_node(ins: list[str], rows: list[str], vals: list[int]) -> int:
    for row in rows:
        parts = row.split()
        pat, out = (parts[0], parts[1]) if len(parts) == 2 else ("", parts[0])
        if out == "1" and all(c == "-" or int(c) == v for c, v in zip(pat, vals, strict=True)):
            return 1
    return 0


def joint(p: float, d: float) -> list[list[float]]:
    return [[1 - p - d / 2, d / 2], [d / 2, p - d / 2]]


def reference(
    text: str, pi_prob: float = 0.5, pi_density: float = 0.2
) -> dict[str, tuple[float, float]]:
    """Per net (P1, density): every node enumerated over its own fanin pair assignments."""
    inputs, nodes = parse(text)
    stat: dict[str, tuple[float, float]] = {n: (pi_prob, pi_density) for n in inputs}
    for ins, out, rows in nodes:  # file order is topological in the tests
        k = len(ins)
        p1 = 0.0
        e11 = 0.0
        for old in itertools.product((0, 1), repeat=k):
            wo = 1.0
            for v, n in zip(old, ins, strict=True):
                wo *= stat[n][0] if v else 1 - stat[n][0]
            fo = eval_node(ins, rows, list(old))
            p1 += wo * fo
            for new in itertools.product((0, 1), repeat=k):
                w = 1.0
                for vo, vn, n in zip(old, new, ins, strict=True):
                    w *= joint(*stat[n])[vo][vn]
                e11 += w * fo * eval_node(ins, rows, list(new))
        stat[out] = (p1, 2 * (p1 - e11))
    return stat

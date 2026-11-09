#!/usr/bin/env python3
"""act_mc: Monte Carlo activity of a combinational or sequential BLIF (the model's ground truth).

Every primary input is an independent two-state Markov chain with static probability P and
transition density D (transition probabilities D/2/(1-P) and D/2/P per cycle); the circuit is
simulated cycle by cycle (``.names`` covers, ``.latch`` from 0) and each net's P1 and density are
the measured ones. Reconvergent fanout and state correlation are therefore included, which the
``activity`` pass does not model: the difference between the two is exactly that cost.

  act_mc.py CIRCUIT.blif [--cycles N] [--seed S] [--pi-prob P] [--pi-density D] [--out F.act]

Standard library only; meant for circuits up to a few thousand nets.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

Cover = tuple[list[str], str, list[tuple[str, str]]]


def parse(text: str) -> tuple[list[str], list[Cover], list[tuple[str, str]], str]:
    """Inputs, covers, latches (input, output) and the clock name."""
    inputs: list[str] = []
    covers: list[Cover] = []
    latches: list[tuple[str, str]] = []
    clock = ""
    cur: Cover | None = None
    lines = text.replace("\\\n", " ").splitlines()
    for raw in lines:
        tok = raw.split("#")[0].split()
        if not tok:
            continue
        if tok[0] == ".inputs":
            inputs += tok[1:]
        elif tok[0] == ".names":
            cur = (tok[1:-1], tok[-1], [])
            covers.append(cur)
        elif tok[0] == ".latch":
            latches.append((tok[1], tok[2]))
            clock = tok[4] if len(tok) > 4 else clock
            cur = None
        elif tok[0] == ".subckt" and tok[1] == "adder":
            pins = dict(word.split("=") for word in tok[2:])
            ins = [pins["a"], pins["b"], pins["cin"]]
            parity = [(f"{n:03b}"[::-1], "1") for n in range(8) if bin(n).count("1") % 2]
            majority = [("11-", "1"), ("1-1", "1"), ("-11", "1")]
            covers.append((ins, pins["sumout"], parity))
            covers.append((ins, pins["cout"], majority))
            cur = None
        elif tok[0].startswith("."):
            cur = None
        elif cur is not None:
            pat, out = (tok[0], tok[1]) if len(tok) == 2 else ("", tok[0])
            cur[2].append((pat, out))
    return inputs, covers, latches, clock


def order(covers: list[Cover], known: set[str]) -> list[Cover]:
    """Covers in dependency order (latch outputs and inputs are known)."""
    pending = list(covers)
    out: list[Cover] = []
    have = set(known)
    while pending:
        ready = [c for c in pending if all(i in have for i in c[0])]
        if not ready:
            raise SystemExit("combinational loop or undriven net")
        for cov in ready:
            out.append(cov)
            have.add(cov[1])
            pending.remove(cov)
    return out


def evaluate(cov: Cover, vals: dict[str, int]) -> int:
    ins, _, rows = cov
    on_set = any(out == "1" for _, out in rows)
    for pat, out in rows:
        if all(c == "-" or int(c) == vals[n] for c, n in zip(pat, ins, strict=True)):
            return 1 if out == "1" else 0
    return 0 if on_set else (1 if rows else 0)


def run(args: argparse.Namespace) -> dict[str, tuple[float, float]]:
    inputs, covers, latches, clock = parse(Path(args.circuit).read_text())
    pis = [n for n in inputs if n != clock]
    latch_out = [q for _, q in latches]
    seq = order(covers, set(pis) | set(latch_out) | {clock, "gnd", "vcc", "unconn"})
    rng = random.Random(args.seed)
    pp, dd = args.pi_prob, args.pi_density
    rise, fall = dd / 2 / (1 - pp), dd / 2 / pp
    vals: dict[str, int] = {"gnd": 0, "vcc": 1, "unconn": 0, clock: 0}
    vals.update({n: 0 for n in latch_out})
    vals.update({n: int(rng.random() < pp) for n in pis})
    nets = [*pis, *latch_out, *(c[1] for c in seq)]
    ones = dict.fromkeys(nets, 0)
    flips = dict.fromkeys(nets, 0)
    last: dict[str, int] = {}
    for cycle in range(args.cycles):
        for n in pis:
            vals[n] = int(rng.random() >= fall) if vals[n] else int(rng.random() < rise)
        for cov in seq:
            vals[cov[1]] = evaluate(cov, vals)
        for n in nets:
            ones[n] += vals[n]
            flips[n] += cycle > 0 and vals[n] != last[n]
            last[n] = vals[n]
        nxt = {q: vals[d] for d, q in latches}
        vals.update(nxt)
    res = {n: (ones[n] / args.cycles, flips[n] / (args.cycles - 1)) for n in nets}
    if clock:
        res[clock] = (0.5, 2.0)
    return res


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("circuit")
    ap.add_argument("--cycles", type=int, default=200000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--pi-prob", type=float, default=0.5)
    ap.add_argument("--pi-density", type=float, default=0.2)
    ap.add_argument("--out")
    args = ap.parse_args()
    res = run(args)
    text = "".join(f"{n} {p:.9g} {d:.9g}\n" for n, (p, d) in res.items())
    if args.out:
        Path(args.out).write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())

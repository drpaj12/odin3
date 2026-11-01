"""vcd-compare: compare two VCD files signal by signal (same time, same value).

Signals are matched by full scope path (tb.dut.q). Every signal of the first file (the odin3
`sim_tb --vcd` file) must exist in the second (an iverilog `$dumpvars(0, tb)` file); the second
may have more (DUT internals). Values are compared at every time at which either file changes the
signal, after reducing each time step to its last value (a glitch inside a step is ignored) and
converting times to femtoseconds with each file's $timescale. A value that is x or z in either
file is skipped (the 4-state start-up of iverilog against the 2-state odin3).
Exit status: 0 identical, 1 mismatch, 2 error.
"""
import argparse
import re
import sys
from collections.abc import Sequence
from pathlib import Path

UNIT_FS = {"s": 10**15, "ms": 10**12, "us": 10**9, "ns": 10**6, "ps": 10**3, "fs": 1}
Timeline = dict[int, int | None]


class VcdError(Exception):
    """A VCD file that cannot be read."""


def parse_timescale(text: str) -> int:
    """Femtoseconds in one time unit of a `$timescale 10 ns $end` body."""
    match = re.fullmatch(r"\s*(\d+)\s*([a-z]+)\s*", text)
    if match is None or match.group(2) not in UNIT_FS:
        raise VcdError(f"bad $timescale `{text.strip()}`")
    return int(match.group(1)) * UNIT_FS[match.group(2)]


def to_value(text: str) -> int | None:
    """The integer of a binary digit string; None when it has an x or z."""
    return None if re.search(r"[xzXZ]", text) else int(text, 2)


def read_vcd(path: Path) -> dict[str, Timeline]:
    """Maps each signal path to {time in fs: last value in that step}."""
    scope: list[str] = []
    codes: dict[str, list[str]] = {}
    series: dict[str, Timeline] = {}
    scale = 0
    now = 0
    tokens = path.read_text().split()
    pos = 0
    while pos < len(tokens):
        tok = tokens[pos]
        pos += 1
        if tok in ("$timescale", "$scope", "$var", "$version", "$date", "$comment"):
            end = tokens.index("$end", pos)
            body = tokens[pos:end]
            pos = end + 1
            if tok == "$timescale":
                scale = parse_timescale(" ".join(body))
            elif tok == "$scope":
                scope.append(body[1])
            elif tok == "$var":
                name = ".".join(scope + [body[3]])
                codes.setdefault(body[2], []).append(name)
                series.setdefault(name, {})
        elif tok == "$upscope":
            scope.pop()
            pos += 1
        elif tok.startswith("#"):
            now = int(tok[1:]) * scale
        elif tok[0] in "bB" and pos < len(tokens):
            code = tokens[pos]
            pos += 1
            for name in codes.get(code, []):
                series[name][now] = to_value(tok[1:])
        elif tok[0] in "01xXzZ" and len(tok) > 1:
            for name in codes.get(tok[1:], []):
                series[name][now] = to_value(tok[0])
    if scale == 0:
        raise VcdError(f"{path}: no $timescale")
    return series


def value_at(line: Timeline, times: list[int], when: int) -> int | None:
    """The last value written at or before `when`; None when there is none (or it is x)."""
    best: int | None = None
    for stamp in times:
        if stamp > when:
            break
        best = line[stamp]
    return best


def read_timescale(path: Path) -> tuple[int, str]:
    """(femtoseconds per unit, the `$timescale` text such as `1 ns`) of a VCD file."""
    tokens = path.read_text().split()
    if "$timescale" not in tokens:
        raise VcdError(f"{path}: no $timescale")
    start = tokens.index("$timescale") + 1
    body = " ".join(tokens[start : tokens.index("$end", start)])
    return parse_timescale(body), body


FS_UNIT = (1, "fs")


def compare_signal(
    name: str, a: Timeline, b: Timeline, unit: tuple[int, str] = FS_UNIT
) -> str | None:
    """A report for the first differing time of one signal, or None. The time is printed in
    `unit` (femtoseconds per tick, label): the reference file's own timescale."""
    ta, tb = sorted(a), sorted(b)
    for when in sorted(set(ta) | set(tb)):
        va, vb = value_at(a, ta, when), value_at(b, tb, when)
        if va is not None and vb is not None and va != vb:
            ticks = when / unit[0]
            return f"{name} at t={ticks:g} x {unit[1]} (={when} fs): {va} vs {vb}"
    return None


def compare(
    a: dict[str, Timeline], b: dict[str, Timeline], unit: tuple[int, str] = FS_UNIT
) -> list[str]:
    """All reports (missing signals, then one per differing signal)."""
    reports = []
    for name in sorted(a):
        if name not in b:
            reports.append(f"{name}: missing from the second file")
            continue
        report = compare_signal(name, a[name], b[name], unit)
        if report is not None:
            reports.append(report)
    return reports


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("a", type=Path, help="odin3 sim_tb --vcd file")
    ap.add_argument("b", type=Path, help="iverilog $dumpvars file")
    args = ap.parse_args(argv)
    try:
        a, b = read_vcd(args.a), read_vcd(args.b)
        unit = read_timescale(args.b)
    except (VcdError, OSError, ValueError, IndexError) as err:
        print(f"vcd-compare: {err}", file=sys.stderr)
        return 2
    if not a:
        print("vcd-compare: the first file has no signals", file=sys.stderr)
        return 2
    reports = compare(a, b, unit)
    if reports:
        print(f"vcd-compare: MISMATCH ({len(reports)} signals)")
        for report in reports[:10]:
            print("  " + report)
        return 1
    print(f"vcd-compare: identical on {len(a)} signals")
    return 0


if __name__ == "__main__":
    sys.exit(main())

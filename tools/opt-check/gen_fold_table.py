"""gen-fold-table: the Yosys 0.55 fold table for 2F's constant folding (OP-5, Task 5).

Usage::

    gen-fold-table --yosys Y --out F [--seed S] [--count N]

Makes N random cells with constant inputs (deterministic per seed), one of every foldable type
in turn, at the widths 1, 7, 64, 65 and 200 (200 at half weight), both signs, with about a third
of the operands holding x/z bits. Each is written as an RTLIL ``cell`` with constant
connections and its output wire ``\\y<k>``, in modules of 256 cells; Yosys runs
``Y -q -p "read_rtlil b.il; opt_expr; write_rtlil o.il"`` and the folded results are read back from
the ``connect \\y<k> <const>`` lines. A Yosys whose ``-V`` does not name 0.55 is refused.

The table starts with ``# gen-fold-table seed S count N`` and ``# yosys: <Y -V>``; each case is
one line ``<type> <a_signed> <b_signed> <y_width> <a> <b> <s> <result>``: the cell type, the
signs, Y_WIDTH, and the operands and the result MSB first in 0 1 x z (``-`` for an operand the
type lacks). For ``$pmux`` b is the concatenated B (S_WIDTH slices of Y_WIDTH bits, slice 0
lowest) and s is S; for ``$mux`` s is the 1-bit S. A_WIDTH, B_WIDTH and S_WIDTH are the operand
lengths. A one-bit operand of a ``$reduce_*`` never holds z: opt_expr's rule for those cells
connects Y to A (z stays z), a rewrite and no fold. ``test_opt_fold`` replays every line
through ``odin3_opt_fold``.

Exit 0 on success, 1 when Yosys fails, 2 on a usage error or a refused Yosys.
"""

from __future__ import annotations

import argparse
import random
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

BATCH = 256
WIDTHS = (1, 7, 64, 65, 200)
# 200-bit lines are long, so they come half as often as the others (the repository's 500 KB file
# limit, the check-added-large-files hook).
WIDTH_CYCLE = (1, 7, 64, 65, 200, 1, 7, 64, 65)
S_WIDTHS = (1, 2, 3, 5)  # $pmux's S_WIDTH
DEFAULT_SEED = 2026
DEFAULT_COUNT = 2500
MIN_COUNT = 2000  # test_opt_fold's floor
XZ_SHARE = 3  # one operand in XZ_SHARE gets x/z bits
YOSYS_TIMEOUT = 600

BINARY = ("$and", "$or", "$xor", "$xnor", "$lt", "$le", "$eq", "$ne", "$gt", "$ge", "$add",
          "$sub", "$mul", "$div", "$mod", "$pow", "$shl", "$shr", "$sshr", "$shiftx",
          "$logic_and", "$logic_or")
UNARY = ("$not", "$neg", "$reduce_and", "$reduce_or", "$reduce_xor", "$reduce_xnor",
         "$reduce_bool", "$logic_not")
MUXES = ("$mux", "$pmux")
# Every foldable type, in the order cases cycle through them.
TYPES = UNARY + BINARY + MUXES
SHIFTS = frozenset({"$shl", "$shr", "$sshr", "$shiftx"})


class GenError(Exception):
    """A failed Yosys run or output that cannot be read."""


class YosysRefused(GenError):
    """A Yosys that cannot be run or is not 0.55."""


@dataclass(frozen=True)
class Case:
    """One table line before Yosys; operands are bit strings, MSB first, None when absent."""

    type: str
    a_signed: bool
    b_signed: bool
    y_width: int
    a: str
    b: str | None
    s: str | None


def _bits(value: int, width: int) -> str:
    return format(value & ((1 << width) - 1), f"0{width}b")


def _value(rng: random.Random, width: int, kind: str) -> int:
    """An operand value; `kind` biases b towards the values its type finds interesting."""
    roll = rng.random()
    if kind == "amount" and roll < 0.7:
        return rng.randint(-2, width + 1)
    if kind == "exponent" and roll < 0.6:
        return rng.randint(-5, 9)
    if kind == "divisor" and roll < 0.15:
        return 0
    if roll < 0.15:
        return rng.randint(-3, 9)
    if roll < 0.25:
        return rng.choice([0, 1, -1, 1 << (width - 1), (1 << (width - 1)) - 1])
    if roll < 0.45:
        return rng.getrandbits(rng.randint(1, width))
    return rng.getrandbits(width)


def _with_xz(rng: random.Random, bits: str) -> str:
    """bits with one to three positions made x or z (MSB first)."""
    out = list(bits)
    for _ in range(rng.randint(1, min(3, len(bits)))):
        out[rng.randrange(len(bits))] = rng.choice("xz")
    return "".join(out)


def _operand(rng: random.Random, width: int, kind: str = "any") -> str:
    bits = _bits(_value(rng, width, kind), width)
    return _with_xz(rng, bits) if rng.randrange(XZ_SHARE) == 0 else bits


def _kind(name: str) -> str:
    if name in SHIFTS:
        return "amount"
    return {"$pow": "exponent", "$div": "divisor", "$mod": "divisor"}.get(name, "any")


def _width(rng: random.Random) -> int:
    return rng.choice(WIDTH_CYCLE)


def _select_bits(rng: random.Random, width: int) -> str:
    """$pmux's S: mostly none or one bit set, sometimes several or an x."""
    roll = rng.random()
    if roll < 0.15:
        return "0" * width
    if roll < 0.65:
        hot = rng.randrange(width)
        return "".join("1" if i == hot else "0" for i in range(width))
    return _operand(rng, width)


def make_case(rng: random.Random, name: str) -> Case:
    y_width = _width(rng)
    if name == "$mux":
        return Case(name, False, False, y_width, _operand(rng, y_width),
                    _operand(rng, y_width), _operand(rng, 1))
    if name == "$pmux":
        slices = rng.choice(S_WIDTHS)
        return Case(name, False, False, y_width, _operand(rng, y_width),
                    _operand(rng, y_width * slices), _select_bits(rng, slices))
    a_signed = rng.random() < 0.5
    if name in UNARY:
        a = _operand(rng, _width(rng))
        if name.startswith("$reduce_") and len(a) == 1:
            a = a.replace("z", "x")  # opt_expr's 1-bit reduce rule aliases Y to A, keeping z
        return Case(name, a_signed, False, y_width, a, None, None)
    b_width = rng.choice((1, 2, 3, 7, 8)) if name in SHIFTS else _width(rng)
    return Case(name, a_signed, rng.random() < 0.5, y_width, _operand(rng, _width(rng)),
                _operand(rng, b_width, _kind(name)), None)


def make_cases(seed: int, count: int) -> list[Case]:
    """count cases, cycling the type; widths, signs and operands from the seed."""
    rng = random.Random(seed)
    return [make_case(rng, TYPES[i % len(TYPES)]) for i in range(count)]


def _const(bits: str) -> str:
    return f"{len(bits)}'{bits}"


def cell_text(k: int, case: Case) -> str:
    """The RTLIL cell computing y<k>, its inputs constants."""
    name = case.type
    params: list[tuple[str, int]] = []
    ports: list[tuple[str, str]] = [("A", _const(case.a))]
    if name in MUXES:
        assert case.b is not None and case.s is not None
        params.append(("WIDTH", case.y_width))
        if name == "$pmux":
            params.append(("S_WIDTH", len(case.s)))
        ports += [("B", _const(case.b)), ("S", _const(case.s))]
    else:
        params.append(("A_SIGNED", int(case.a_signed)))
        if case.b is not None:
            params.append(("B_SIGNED", int(case.b_signed)))
        params.append(("A_WIDTH", len(case.a)))
        if case.b is not None:
            params.append(("B_WIDTH", len(case.b)))
            ports.append(("B", _const(case.b)))
        params.append(("Y_WIDTH", case.y_width))
    ports.append(("Y", f"\\y{k}"))
    lines = [f"  cell {name} \\c{k}"]
    lines += [f"    parameter \\{p} {v}" for p, v in params]
    lines += [f"    connect \\{p} {v}" for p, v in ports]
    lines.append("  end")
    return "\n".join(lines)


def module_text(cases: Sequence[Case]) -> str:
    wires = "\n".join(
        f"  wire width {c.y_width} output {k + 1} \\y{k}" for k, c in enumerate(cases))
    cells = "\n".join(cell_text(k, c) for k, c in enumerate(cases))
    return f"module \\b\n{wires}\n{cells}\nend\n"


_CONNECT = re.compile(r"^\s*connect\s+(\S+)\s+(.*?)\s*$")
_CONST = re.compile(r"(\d+)'(s?)([01xzm-]+)")


def _chunk(text: str) -> str:
    """One RTLIL constant chunk as bits, MSB first: `8'1x0z0000`, `4'x`, `3's101` or `5`."""
    if text.isdigit():
        return _bits(int(text), 32)
    match = _CONST.fullmatch(text)
    if match is None:
        raise GenError(f"not a constant: {text!r}")
    width, digits = int(match.group(1)), match.group(3)
    if digits == "x" and width != 1:
        return "x" * width
    if len(digits) != width or re.search("[m-]", digits):
        raise GenError(f"unexpected constant: {text!r}")
    return digits


def _split(text: str) -> list[str]:
    text = text.strip()
    if text.startswith("{") and text.endswith("}"):
        return text[1:-1].split()
    return [text]


def parse_const(text: str, wires: dict[str, str] | None = None) -> str:
    """An RTLIL constant sigspec (one chunk, or `{ … }` of chunks MSB first) as bits. A chunk
    that names a wire in `wires` (name -> its bits, MSB first) is replaced by those bits."""
    parts = []
    for chunk in _split(text):
        if wires is not None and chunk in wires:
            parts.append(wires[chunk])
        else:
            parts.append(_chunk(chunk))
    return "".join(parts)


def parse_connects(rtlil: str) -> dict[int, str]:
    """`connect \\y<k> <const>` lines of an RTLIL dump: k -> the constant's bits, MSB first. Yosys
    leaves a result bit it built through a cell on a temporary wire (`connect \\y0 { 64'0 $auto$..
    }` with `connect $auto$.. 1'1`); such a whole wire is resolved to the constant it is
    connected to."""
    connects: dict[str, str] = {}
    for line in rtlil.splitlines():
        match = _CONNECT.match(line)
        if match is not None:
            connects[match.group(1)] = match.group(2)
    temps = {name: parse_const(text) for name, text in connects.items()
             if not name.startswith("\\y") and _is_const(text)}
    found: dict[int, str] = {}
    for name, text in connects.items():
        match = re.fullmatch(r"\\y(\d+)", name)
        if match is not None:
            found[int(match.group(1))] = parse_const(text, temps)
    return found


def _is_const(text: str) -> bool:
    try:
        parse_const(text)
    except GenError:
        return False
    return True


def yosys_version(yosys: str) -> str:
    """`yosys -V`'s line; YosysRefused unless it names Yosys 0.55."""
    try:
        run = subprocess.run([yosys, "-V"], capture_output=True, text=True, check=False,
                             timeout=YOSYS_TIMEOUT)
    except OSError as exc:
        raise YosysRefused(f"cannot run {yosys}: {exc}") from exc
    version = run.stdout.strip()
    if run.returncode != 0 or not re.search(r"\b0\.55\b", version):
        found = version or "unknown"
        raise YosysRefused(f"{yosys} is {found!r}; the fold table needs Yosys 0.55")
    return version


def run_batch(yosys: str, cases: Sequence[Case], workdir: Path) -> list[str]:
    """The results of one batch, in case order."""
    (workdir / "b.il").write_text(module_text(cases))
    script = "read_rtlil b.il; opt_expr; write_rtlil o.il"
    run = subprocess.run([yosys, "-q", "-p", script], cwd=workdir, capture_output=True,
                         text=True, check=False, timeout=YOSYS_TIMEOUT)
    if run.returncode != 0:
        raise GenError(f"yosys failed ({run.returncode}): {run.stderr.strip()[-2000:]}")
    found = parse_connects((workdir / "o.il").read_text())
    results: list[str] = []
    for k, case in enumerate(cases):
        bits = found.get(k)
        if bits is None or len(bits) != case.y_width:
            raise GenError(f"y{k} ({case.type}): no {case.y_width}-bit constant")
        results.append(bits)
    return results


def case_line(case: Case, result: str) -> str:
    return (f"{case.type} {int(case.a_signed)} {int(case.b_signed)} {case.y_width} {case.a} "
            f"{case.b or '-'} {case.s or '-'} {result}")


def table_text(header: Sequence[str], cases: Sequence[Case], results: Sequence[str]) -> str:
    lines = list(header)
    lines += [case_line(case, result) for case, result in zip(cases, results, strict=True)]
    return "\n".join(lines) + "\n"


def generate(yosys: str, seed: int, count: int) -> str:
    version = yosys_version(yosys)
    cases = make_cases(seed, count)
    results: list[str] = []
    with tempfile.TemporaryDirectory(prefix="gen-fold-table-") as tmp:
        for start in range(0, len(cases), BATCH):
            results += run_batch(yosys, cases[start:start + BATCH], Path(tmp))
    header = [f"# gen-fold-table seed {seed} count {count}", f"# yosys: {version}",
              "# <type> <a_signed> <b_signed> <y_width> <a> <b> <s> <result>: MSB first in",
              "# 0 1 x z, '-' for an operand the type lacks; $pmux's b is B, S_WIDTH slices"]
    return table_text(header, cases, results)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="gen-fold-table", description=__doc__.split("\n")[0])
    parser.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    parser.add_argument("--out", required=True, type=Path, help="the table to write")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--count", type=int, default=DEFAULT_COUNT)
    args = parser.parse_args(argv)
    if args.count < 1:
        parser.error("--count must be at least 1")
    try:
        text = generate(args.yosys, args.seed, args.count)
    except YosysRefused as exc:
        print(f"gen-fold-table: {exc}", file=sys.stderr)
        return 2
    except GenError as exc:
        print(f"gen-fold-table: {exc}", file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired as exc:
        print(f"gen-fold-table: yosys timed out after {exc.timeout} s", file=sys.stderr)
        return 1
    args.out.write_text(text)
    print(f"gen-fold-table: {args.count} cases -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

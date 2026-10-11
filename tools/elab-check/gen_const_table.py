"""gen-const-table: the Yosys 0.55 constant table for 2D's operators (EL-14, Task 7).

Usage::

    gen-const-table --yosys Y --out F [--seed S] [--count N]

Makes N random cases (deterministic per seed) covering every operator of ``odin3_cop`` at the
widths 1, 7, 64, 65 and 200 (200 at half weight, ``WIDTH_CYCLE``), unsigned and signed, with about
a third of the operands holding x/z bits. The cases go to Yosys in modules of 256 outputs each,
``assign y<k> = <expr>;`` with both operands written as sized binary literals at the case's width
and sign, and the folded results are read back from ``write_rtlil``'s ``connect \\y<k> <const>``
lines (``localparam``s do not appear in RTLIL; plan point (z)). Yosys runs as ``Y -q -p
"read_verilog -sv b.v; write_rtlil b.il"``; one whose ``-V`` does not name 0.55 is refused (the
oracle is Yosys 0.55 only).

The table starts with ``# gen-const-table seed S count N`` and ``# yosys: <Y -V>``; each case is
one line ``<op> <width> <s> <a> <b> <result>``: the operator's ``odin3_cop`` name, the width, 1
for signed, and the operands and the result MSB first in 0 1 x z (``-`` for a unary operator's
missing b). ``test_elab_cval`` replays every line through ``odin3_cval_op``.

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
# The order cases take their widths in. 200-bit lines are ~600 bytes, so they come half as often
# as the others: the default table then stays under the repository's 500 KB file limit (the
# check-added-large-files pre-commit hook).
WIDTH_CYCLE = (1, 7, 64, 65, 200, 1, 7, 64, 65)
DEFAULT_SEED = 2026
DEFAULT_COUNT = 3060  # 90 cases per operator
MIN_COUNT = 3000  # test_elab_cval's floor
XZ_SHARE = 3  # one operand in XZ_SHARE gets x/z bits
YOSYS_TIMEOUT = 600


@dataclass(frozen=True)
class Op:
    """An operator: its Verilog spelling, its arity and how its b operand is drawn."""

    symbol: str
    arity: int
    b_kind: str = "any"  # "any", "amount" (shift), "exponent" (pow), "divisor"


# In odin3_cop order (src/frontends/elab/cval.h).
OPS: dict[str, Op] = {
    "ADD": Op("+", 2),
    "SUB": Op("-", 2),
    "MUL": Op("*", 2),
    "DIV": Op("/", 2, "divisor"),
    "MOD": Op("%", 2, "divisor"),
    "POW": Op("**", 2, "exponent"),
    "AND": Op("&", 2),
    "OR": Op("|", 2),
    "XOR": Op("^", 2),
    "XNOR": Op("~^", 2),
    "SHL": Op("<<", 2, "amount"),
    "SHR": Op(">>", 2, "amount"),
    "SSHL": Op("<<<", 2, "amount"),
    "SSHR": Op(">>>", 2, "amount"),
    "EQ": Op("==", 2),
    "NE": Op("!=", 2),
    "EQX": Op("===", 2),
    "NEX": Op("!==", 2),
    "LT": Op("<", 2),
    "LE": Op("<=", 2),
    "GT": Op(">", 2),
    "GE": Op(">=", 2),
    "LAND": Op("&&", 2),
    "LOR": Op("||", 2),
    "NEG": Op("-", 1),
    "POS": Op("+", 1),
    "NOT": Op("~", 1),
    "LNOT": Op("!", 1),
    "RAND": Op("&", 1),
    "RNAND": Op("~&", 1),
    "ROR": Op("|", 1),
    "RNOR": Op("~|", 1),
    "RXOR": Op("^", 1),
    "RXNOR": Op("~^", 1),
}


class GenError(Exception):
    """A failed Yosys run or output that cannot be read."""


class YosysRefused(GenError):
    """A Yosys that cannot be run or is not 0.55."""


@dataclass(frozen=True)
class Case:
    """One table line before Yosys: operator, width, sign and the operands (MSB first)."""

    op: str
    width: int
    signed: bool
    a: str
    b: str | None


def _bits(value: int, width: int) -> str:
    return format(value & ((1 << width) - 1), f"0{width}b")


def _value(rng: random.Random, width: int, kind: str) -> int:
    """An operand value; `kind` biases b towards the values its operator finds interesting."""
    roll = rng.random()
    if kind == "amount" and roll < 0.7:
        return rng.randint(0, width + 1)
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
    """bits with one to three positions made x or z (MSB first, so position 0 is the MSB)."""
    out = list(bits)
    for _ in range(rng.randint(1, min(3, len(bits)))):
        out[rng.randrange(len(bits))] = rng.choice("xz")
    return "".join(out)


def _operand(rng: random.Random, width: int, kind: str) -> str:
    bits = _bits(_value(rng, width, kind), width)
    return _with_xz(rng, bits) if rng.randrange(XZ_SHARE) == 0 else bits


def make_cases(seed: int, count: int) -> list[Case]:
    """count cases, cycling operator fastest, then width, then sign; operands from the seed."""
    rng = random.Random(seed)
    names = list(OPS)
    cases: list[Case] = []
    for i in range(count):
        name = names[i % len(names)]
        width = WIDTH_CYCLE[(i // len(names)) % len(WIDTH_CYCLE)]
        signed = (i // (len(names) * len(WIDTH_CYCLE))) % 2 == 1
        op = OPS[name]
        a = _operand(rng, width, "any")
        b = _operand(rng, width, op.b_kind) if op.arity == 2 else None
        cases.append(Case(name, width, signed, a, b))
    return cases


def literal(bits: str, signed: bool) -> str:
    return f"{len(bits)}'{'s' if signed else ''}b{bits}"


def expression(case: Case) -> str:
    op = OPS[case.op]
    a = literal(case.a, case.signed)
    if case.b is None:
        return f"{op.symbol}{a}"
    return f"{a} {op.symbol} {literal(case.b, case.signed)}"


def module_text(cases: Sequence[Case]) -> str:
    ports = ",\n".join(f"  output [{c.width - 1}:0] y{k}" for k, c in enumerate(cases))
    body = "\n".join(f"assign y{k} = {expression(c)};" for k, c in enumerate(cases))
    return f"module b(\n{ports}\n);\n{body}\nendmodule\n"


_CONNECT = re.compile(r"^\s*connect\s+\\y(\d+)\s+(.*?)\s*$")
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


def parse_const(text: str) -> str:
    """An RTLIL constant sigspec (one chunk, or `{ … }` of chunks MSB first) as bits."""
    text = text.strip()
    if text.startswith("{") and text.endswith("}"):
        return "".join(_chunk(part) for part in text[1:-1].split())
    return _chunk(text)


def parse_connects(rtlil: str) -> dict[int, str]:
    """`connect \\y<k> <const>` lines of an RTLIL dump: k -> the constant's bits, MSB first."""
    found: dict[int, str] = {}
    for line in rtlil.splitlines():
        match = _CONNECT.match(line)
        if match is not None:
            found[int(match.group(1))] = parse_const(match.group(2))
    return found


def yosys_version(yosys: str) -> str:
    """`yosys -V`'s line; GenError unless it names Yosys 0.55."""
    try:
        run = subprocess.run([yosys, "-V"], capture_output=True, text=True, check=False,
                             timeout=YOSYS_TIMEOUT)
    except OSError as exc:
        raise YosysRefused(f"cannot run {yosys}: {exc}") from exc
    version = run.stdout.strip()
    if run.returncode != 0 or not re.search(r"\b0\.55\b", version):
        found = version or "unknown"
        raise YosysRefused(f"{yosys} is {found!r}; the constant table needs Yosys 0.55")
    return version


def run_batch(yosys: str, cases: Sequence[Case], workdir: Path) -> list[str]:
    """The results of one batch, in case order."""
    (workdir / "b.v").write_text(module_text(cases))
    script = "read_verilog -sv b.v; write_rtlil b.il"
    run = subprocess.run([yosys, "-q", "-p", script], cwd=workdir, capture_output=True,
                         text=True, check=False, timeout=YOSYS_TIMEOUT)
    if run.returncode != 0:
        raise GenError(f"yosys failed ({run.returncode}): {run.stderr.strip()[-2000:]}")
    found = parse_connects((workdir / "b.il").read_text())
    results: list[str] = []
    for k, case in enumerate(cases):
        bits = found.get(k)
        if bits is None or len(bits) != case.width:
            raise GenError(f"y{k} ({expression(case)}): no {case.width}-bit constant in b.il")
        results.append(bits)
    return results


def table_text(header: Sequence[str], cases: Sequence[Case], results: Sequence[str]) -> str:
    lines = list(header)
    for case, result in zip(cases, results, strict=True):
        sign = "1" if case.signed else "0"
        lines.append(f"{case.op} {case.width} {sign} {case.a} {case.b or '-'} {result}")
    return "\n".join(lines) + "\n"


def generate(yosys: str, seed: int, count: int) -> str:
    version = yosys_version(yosys)
    cases = make_cases(seed, count)
    results: list[str] = []
    with tempfile.TemporaryDirectory(prefix="gen-const-table-") as tmp:
        for start in range(0, len(cases), BATCH):
            results += run_batch(yosys, cases[start:start + BATCH], Path(tmp))
    header = [f"# gen-const-table seed {seed} count {count}", f"# yosys: {version}",
              "# <op> <width> <s> <a> <b> <result>: MSB first in 0 1 x z, '-' for no b"]
    return table_text(header, cases, results)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="gen-const-table", description=__doc__.split("\n")[0])
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
        print(f"gen-const-table: {exc}", file=sys.stderr)
        return 2
    except GenError as exc:
        print(f"gen-const-table: {exc}", file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired as exc:
        print(f"gen-const-table: yosys timed out after {exc.timeout} s", file=sys.stderr)
        return 1
    args.out.write_text(text)
    print(f"gen-const-table: {args.count} cases -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

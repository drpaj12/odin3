"""cone_check_micros: `cone_check` end to end on small netlist micros, through the odin3 binary.

Usage: cone_check_micros.py ODIN3 OUT_DIR

Each micro is a BLIF text (generated here) read with `read_blif`, written as JSON (`write_json
--odin3`), marked in the JSON (every logic cell gets attribute `grp`, every one-bit port wire gets
`odin3:bind`), read back and run through `cone_check`; the verdict is read from the module
attributes of a second `write_json --odin3`. Phase 7 spec 3.3 C-5: a ripple adder cone against `$add`
is equivalent (4 and 8 bits), a bitwise AND cone against `$and` is equivalent, a swapped binding
differs with a counterexample, a forced 0 s timeout is unproven, a group with a register is
refused. Exit 0 when every micro gives its expected verdict, 1 otherwise, 2 on a usage error.
"""

from __future__ import annotations

import json
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

TECHLIB = Path(__file__).resolve().parents[2] / "lib" / "vtr.o3lib"
GROUP = "cone"


@dataclass
class Micro:
    """One case: the BLIF, the binding (net -> template bit), the pass arguments, the expectation."""

    name: str
    blif: str
    binding: dict[str, str]
    cell: str
    expect: str  # equivalent | differs | unproven | refused
    extra: str = ""
    refusal: str = ""
    params: list[str] = field(default_factory=list)


def xor3(a: str, b: str, c: str, out: str) -> str:
    return f".names {a} {b} {c} {out}\n001 1\n010 1\n100 1\n111 1\n"


def ripple(width: int) -> tuple[str, dict[str, str]]:
    """A half adder at bit 0 and full adders above; the last carry is unused."""
    ins = [f"a{i}" for i in range(width)] + [f"b{i}" for i in range(width)]
    outs = [f"s{i}" for i in range(width)]
    text = f".model m\n.inputs {' '.join(ins)}\n.outputs {' '.join(outs)}\n"
    text += ".names a0 b0 s0\n10 1\n01 1\n.names a0 b0 c1\n11 1\n"
    for i in range(1, width):
        text += xor3(f"a{i}", f"b{i}", f"c{i}", f"s{i}")
        if i + 1 < width:
            text += f".names a{i} b{i} c{i} c{i + 1}\n11- 1\n1-1 1\n-11 1\n"
    binding = {f"a{i}": f"A[{i}]" for i in range(width)}
    binding.update({f"b{i}": f"B[{i}]" for i in range(width)})
    binding.update({f"s{i}": f"Y[{i}]" for i in range(width)})
    return text + ".end\n", binding


def and_bits(width: int) -> tuple[str, dict[str, str]]:
    ins = [f"a{i}" for i in range(width)] + [f"b{i}" for i in range(width)]
    outs = [f"y{i}" for i in range(width)]
    text = f".model m\n.inputs {' '.join(ins)}\n.outputs {' '.join(outs)}\n"
    text += "".join(f".names a{i} b{i} y{i}\n11 1\n" for i in range(width)) + ".end\n"
    binding = {f"a{i}": f"A[{i}]" for i in range(width)}
    binding.update({f"b{i}": f"B[{i}]" for i in range(width)})
    binding.update({f"y{i}": f"Y[{i}]" for i in range(width)})
    return text, binding


def incrementer(width: int) -> tuple[str, dict[str, str]]:
    """s = a + 1 modulo 2**width: s0 = !a0, s_i = a_i ^ c_i, c_1 = a0, c_(i+1) = a_i & c_i."""
    ins = [f"a{i}" for i in range(width)]
    outs = [f"s{i}" for i in range(width)]
    text = f".model m\n.inputs {' '.join(ins)}\n.outputs {' '.join(outs)}\n"
    text += ".names a0 s0\n0 1\n"
    for i in range(1, width):
        carry = "a0" if i == 1 else f"c{i}"
        text += f".names a{i} {carry} s{i}\n01 1\n10 1\n"
        if i + 1 < width:
            text += f".names a{i} {carry} c{i + 1}\n11 1\n"
    binding = {f"a{i}": f"A[{i}]" for i in range(width)}
    binding.update({f"s{i}": f"Y[{i}]" for i in range(width)})
    return text + ".end\n", binding


def eq_const(width: int, value: int) -> tuple[str, dict[str, str]]:
    """y = (a == value): one row, bit i of value for input a_i."""
    ins = [f"a{i}" for i in range(width)]
    row = "".join(str((value >> i) & 1) for i in range(width))
    text = f".model m\n.inputs {' '.join(ins)}\n.outputs y\n"
    text += f".names {' '.join(ins)} y\n{row} 1\n.end\n"
    binding = {f"a{i}": f"A[{i}]" for i in range(width)}
    binding["y"] = "Y[0]"
    return text, binding


def const_micros() -> list[Micro]:
    """The -const micros (v1.0.5 plan Task 2): a constant operand of the template."""
    out: list[Micro] = []
    text, binding = incrementer(4)
    inc = [f"-param {p}" for p in ("A_WIDTH=4", "B_WIDTH=1", "Y_WIDTH=4")] + ["-const B=1"]
    out.append(Micro("inc4_vs_add_const", text, binding, "$add", "equivalent", params=inc))
    text, binding = eq_const(4, 13)
    eq = [f"-param {p}" for p in ("A_WIDTH=4", "B_WIDTH=4", "Y_WIDTH=1")]
    out.append(Micro("eq13_vs_eq_const", text, binding, "$eq", "equivalent",
                     params=[*eq, "-const B=13"]))
    out.append(Micro("eq13_wrong_const", text, binding, "$eq", "differs",
                     params=[*eq, "-const B=12"]))
    out.append(Micro("const_output_refused", text, binding, "$eq", "refused",
                     refusal="is not an input", params=[*eq, "-const Y=1"]))
    return out


def micros() -> list[Micro]:
    out: list[Micro] = []
    for width in (4, 8):
        text, binding = ripple(width)
        params = [f"-param {p}_WIDTH={width}" for p in ("A", "B", "Y")]
        out.append(Micro(f"ripple{width}_vs_add", text, binding, "$add", "equivalent",
                         params=params))
    text, binding = ripple(4)
    swapped = dict(binding)
    swapped["a0"], swapped["a1"] = binding["a1"], binding["a0"]
    add4 = [f"-param {p}_WIDTH=4" for p in ("A", "B", "Y")]
    out.append(Micro("ripple4_swapped", text, swapped, "$add", "differs", params=add4))
    out.append(Micro("ripple4_timeout0", text, binding, "$add", "unproven", extra="-timeout 0",
                     params=add4))
    text, binding = and_bits(3)
    out.append(Micro("and3_vs_and", text, binding, "$and", "equivalent",
                     params=[f"-param {p}_WIDTH=3" for p in ("A", "B", "Y")]))
    reg = ".model m\n.inputs a b clk\n.outputs q\n.names a b d\n11 1\n.latch d q re clk 0\n.end\n"
    out.append(Micro("register_refused", reg, {"a": "A[0]", "b": "A[1]", "q": "Y[0]"}, "$and",
                     "refused", refusal="register", params=["-param A_WIDTH=2"]))
    return out + const_micros()


def odin3(binary: str, script: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([binary, "-p", script], capture_output=True, text=True, check=False)


def mark(src: Path, dst: Path, micro: Micro) -> None:
    design = json.loads(src.read_text(encoding="utf-8"))
    module = next(iter(design["modules"].values()))
    for cell in module["cells"].values():
        cell["attributes"]["grp"] = GROUP
    for net, bit in micro.binding.items():
        module["netnames"][net]["attributes"]["odin3:bind"] = bit
    dst.write_text(json.dumps(design), encoding="utf-8")


def run_micro(binary: str, work: Path, micro: Micro) -> str:
    """'' when the micro gives its expected result, else the reason."""
    work.mkdir(parents=True, exist_ok=True)
    blif, plain, marked, result = (work / n for n in ("t.blif", "a.json", "b.json", "r.json"))
    blif.write_text(micro.blif, encoding="utf-8")
    first = odin3(binary, f"read_techlib {TECHLIB}; read_blif {blif}; write_json --odin3 {plain}")
    if first.returncode != 0:
        return "could not read the micro: " + first.stderr.strip()[-200:]
    mark(plain, marked, micro)
    args = " ".join([f"-attr grp={GROUP}", f"-cell {micro.cell}", *micro.params, micro.extra])
    second = odin3(binary, f"read_techlib {TECHLIB}; read_json --top m {marked}; "
                           f"cone_check {args}; write_json --odin3 {result}")
    if micro.expect == "refused":
        text = second.stderr + second.stdout
        ok = second.returncode != 0 and micro.refusal in text
        return "" if ok else "expected a refusal: " + text.strip()[-200:]
    if second.returncode != 0:
        return "cone_check failed: " + second.stderr.strip()[-300:]
    attrs = next(iter(json.loads(result.read_text(encoding="utf-8"))["modules"].values()))[
        "attributes"]
    verdict = attrs.get("odin3:cone_check")
    if verdict != micro.expect:
        return f"verdict {verdict!r}, expected {micro.expect!r}"
    cex = attrs.get("odin3:cone_cex")
    if (micro.expect == "differs") != (cex is not None):
        return f"counterexample {cex!r} does not match the verdict"
    if "odin3:cone_hash" not in attrs or attrs.get("odin3:cone_key") != f"grp={GROUP}":
        return "the hash or key attribute is missing"
    return ""


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    binary, out_dir = argv[1], Path(argv[2])
    failed = 0
    cases = micros()
    for micro in cases:
        reason = run_micro(binary, out_dir / micro.name, micro)
        failed += 1 if reason else 0
        print(f"{micro.name} {micro.expect if not reason else 'FAIL: ' + reason}", flush=True)
    print(f"cone_check_micros: {len(cases)} micros, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

"""lpm-check: one LPM micro through the script, the Verilog round trip and the lint.

5.1 spec T-1..T-4.

Usage::

    lpm-check [--odin3 P] [--lib L] [--script S] [--expect F] [--family NAME] [--keep DIR]
              [--lint-only] MICRO.v

Legs, in order (the first failure is printed and the exit status is 1):

1. ``counts``: Odin III runs the script (``tests/lpm/lpm.script``) on the micro and writes
   Verilog; the INFO line ``lpmmap: N lpm_add_sub, ...`` must equal the ``counts`` row of the
   expect file, and every ``param`` row must be found on an instance of its type.
   Rows ``pin <micro> <type> <pin> <value>`` do the same for a pin connection, and ``noroundtrip
   <micro>`` skips the round trip and ``nolint <micro>`` the lint (a micro that keeps ``$mem``
   cells: the writer emits them without a stub and only ``read_verilog -icells``, item 5.2,
   reads them). Every written instance is also checked for the defaults of spec M1: an input pin
   that has a documented default must be tied, never open and never ``1'bx``
   (``DEFAULTED`` below; ``add_sub`` is exempt, it cannot be used with ``lpm_direction``).
   The same script then writes BLIF (``write_blif``, PLAIN dialect; the LPM parameters the reader
   cannot derive are ``.param`` lines, PHASE5 decision 8), which is read back with the LPM library
   and written again: the two files must be identical and ``netlist-compare`` must say identical
   (the models of the LPM cells have no ``.model`` stanza, so black-box stubs built from
   ``lib/lpm.o3lib`` are appended to copies first).
2. ``roundtrip`` (spec T-3): the Verilog is read back with the LPM library and written again
   (generation 2), then again (generation 3). Reading turns an open instance pin into the
   constant x (inputs) or a fresh net (outputs), so generation 2 differs from generation 1 in
   exactly those connections; the checks are therefore: generation 3 equals generation 2 once
   the ``// file:line`` comments are dropped (a fixpoint), and every instance of generation 1 has
   the same type, name, parameters and connected pins in generation 2, an open pin, an x pin and
   a pin on a fresh ``$n<ID>`` net counting as equal. The read-back also runs ``--check`` (the IR
   invariants).
3. ``lint`` (spec T-4): ``iverilog -g2005 -Wall`` must report nothing but the floating-input
   notes (the LPM defaults of unconnected inputs are the point, spec H-4), and Verilator, when
   installed, must pass ``--lint-only -Wall`` apart from the empty-pin warnings. Both read the
   stub modules the writer emits, so a port width that disagrees with its parameter is a finding.

``--lint-only`` runs only the lint leg on MICRO.v taken as already written Verilog (for the
planted-mismatch test). Exit 77 when ``iverilog`` is not installed.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LIB = ROOT / "lib" / "lpm.o3lib"
DEFAULT_SCRIPT = ROOT / "tests" / "lpm" / "lpm.script"
DEFAULT_EXPECT = ROOT / "tests" / "lpm" / "expect.txt"
DEFAULT_COMPARE = ROOT / "tools" / "netlist-compare" / "netlist-compare"
TIMEOUT = 120
LPM_TYPES = ("lpm_add_sub", "lpm_mult", "lpm_compare", "altsyncram")

INFO_RE = re.compile(
    r"lpmmap: (\d+) lpm_add_sub, (\d+) lpm_mult, (\d+) lpm_compare, (\d+) altsyncram; "
    r"(\d+) cells left soft"
)
INSTANCE_RE = re.compile(
    r"^\s*(?:\(\*[^*]*\*\)\s*)?(lpm_add_sub|lpm_mult|lpm_compare|altsyncram)\s+#\((.*?)\)\s+"
    r"(\S+)\s*\(",
    re.M,
)
PARAM_RE = re.compile(r"\.(\w+)\(((?:\"[^\"]*\")|[^,)]*)\)")
PIN_RE = re.compile(r"^\s*\.(\w+)\((.*)\),?\s*$")
# What an open pin becomes on read-back: the constant x (an input) or a fresh net named $n<ID>
# (an output; a vector of them is a concatenation).
OPEN_PIN = re.compile(r"^(?:1'bx|\\\$n\d+ ?|\{(?:\\\$n\d+ ?,? ?)+\})$")
FLOATING = re.compile(r"dangling input port .* floating")


# The input pins that have a documented default and are tied by lpmmap (spec M1, UG-01063; the
# altsyncram list is R-3/R-4's ties). An open or x pin here is a finding.
DEFAULTED: dict[str, tuple[str, ...]] = {
    "lpm_add_sub": ("cin", "clken", "aclr", "clock"),
    "lpm_mult": ("sum", "clken", "aclr", "clock"),
    "lpm_compare": ("clken", "aclr", "clock"),
    "altsyncram": (
        "clock1",
        "address_a",
        "data_a",
        "rden_a",
        "wren_a",
        "addressstall_a",
        "address_b",
        "data_b",
        "rden_b",
        "wren_b",
        "addressstall_b",
        "clocken0",
        "clocken1",
        "clocken2",
        "clocken3",
        "aclr0",
        "aclr1",
    ),
}


class CheckError(Exception):
    """A leg failed (exit 1) or the tool could not run (exit 2, usage=True)."""

    def __init__(self, message: str, usage: bool = False) -> None:
        super().__init__(message)
        self.usage = usage


def strip_comments(text: str) -> str:
    """Drops the trailing ``// ...`` comment of every line (the provenance locations)."""
    return re.sub(r"[ \t]*//.*$", "", text, flags=re.M)


def run(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=TIMEOUT, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise CheckError(f"cannot run {cmd[0]}: {exc}", usage=True) from exc


def odin3_run(
    odin3: str, script: str, extra: list[str] | None = None
) -> subprocess.CompletedProcess[str]:
    proc = run([odin3, *(extra or []), "-p", script])
    if proc.returncode != 0:
        tail = "\n".join(proc.stderr.strip().splitlines()[-5:])
        raise CheckError(f"odin3 failed ({proc.returncode}) on: {script}\n{tail}")
    return proc


def script_text(script: Path, lib: Path, design: Path, out: Path, family: str | None) -> str:
    lines = [ln for ln in script.read_text().splitlines() if ln.strip() and not ln.startswith("#")]
    if len(lines) != 1:
        raise CheckError(f"{script}: expected one script line, found {len(lines)}", usage=True)
    fam = f'--family "{family}"' if family else ""
    text = lines[0].replace("@LIB@", str(lib)).replace("@DESIGN@", str(design))
    return text.replace("@FAMILY@", fam).replace("@OUT@", str(out)).replace("  ", " ")


# --- the expect file ---------------------------------------------------------------------------


class Expect:
    """The rows of the expect file for one micro."""

    def __init__(self) -> None:
        self.counts: list[int] | None = None
        self.params: list[tuple[str, str, str]] = []
        self.pins: list[tuple[str, str, str]] = []
        self.roundtrip = True
        self.lint = True


def read_expect(path: Path, micro: str) -> Expect:
    exp = Expect()
    for raw in path.read_text().splitlines():
        parts = raw.split(None, 4)
        if len(parts) < 2 or parts[0].startswith("#") or parts[1] != micro:
            continue
        if parts[0] == "counts":
            exp.counts = [int(n) for n in raw.split()[2:]]
        elif parts[0] == "param":
            exp.params.append((parts[2], parts[3], parts[4]))
        elif parts[0] == "pin":
            exp.pins.append((parts[2], parts[3], parts[4]))
        elif parts[0] == "noroundtrip":
            exp.roundtrip = False
        elif parts[0] == "nolint":
            exp.lint = False
    return exp


# --- parsing the written Verilog ----------------------------------------------------------------


def instances(text: str) -> list[tuple[str, dict[str, str], str, dict[str, str]]]:
    """(type, parameters, instance name, connected pins) of every LPM instance, in file order."""
    found = []
    for match in INSTANCE_RE.finditer(text):
        kind, plist, name = match.group(1), match.group(2), match.group(3)
        params = {m.group(1): m.group(2).strip() for m in PARAM_RE.finditer(plist)}
        start = match.end()
        end = text.index(");", start)
        pins: dict[str, str] = {}
        for line in text[start:end].splitlines():
            pm = PIN_RE.match(line)
            if pm and pm.group(2).strip() and not OPEN_PIN.match(pm.group(2).strip()):
                pins[pm.group(1)] = pm.group(2).strip()
        found.append((kind, params, name, pins))
    return found


def check_defaults(text: str, label: str) -> None:
    """Every defaulted input pin of every LPM instance is tied (spec M1)."""
    for kind, _params, name, pins in instances(text):
        for pin in DEFAULTED.get(kind, ()):
            if pin not in pins:
                raise CheckError(f"defaults: {label}: {kind} {name}: input .{pin}() is open or x")


def check_counts(
    text: str,
    info: str,
    counts: list[int] | None,
    params: list[tuple[str, str, str]],
    pins: list[tuple[str, str, str]] | None = None,
) -> None:
    match = INFO_RE.search(info)
    if match is None:
        raise CheckError("counts: no 'lpmmap: ...' INFO line in the odin3 output")
    got = [int(match.group(i)) for i in range(1, 6)]
    if counts is not None and got != counts:
        raise CheckError(f"counts: lpmmap made {got}, the expect file says {counts}")
    seen = instances(text)
    for kind, name, value in params:
        if not any(k == kind and p.get(name) == value for k, p, _, _ in seen):
            raise CheckError(f"counts: no {kind} instance with .{name}({value})")
    for kind, pin, value in pins or []:
        if not any(k == kind and q.get(pin) == value for k, _, _, q in seen):
            raise CheckError(f"counts: no {kind} instance with .{pin}({value})")
    for kind, pin, value in pins or []:
        if not any(k == kind and q.get(pin) == value for k, _, _, q in seen):
            raise CheckError(f"counts: no {kind} instance with .{pin}({value})")
    for kind in LPM_TYPES:
        n_inst = sum(1 for k, _, _, _ in seen if k == kind)
        if n_inst != got[LPM_TYPES.index(kind)]:
            raise CheckError(
                f"counts: {n_inst} {kind} instances written, INFO line says "
                f"{got[LPM_TYPES.index(kind)]}"
            )


# --- the round trip -----------------------------------------------------------------------------


def read_back(odin3: str, lib: Path, source: Path, out: Path) -> str:
    odin3_run(odin3, f"read_techlib {lib}; read_verilog {source}; write_verilog {out}", ["--check"])
    return out.read_text()


def check_roundtrip(odin3: str, lib: Path, v1: Path, work: Path) -> None:
    v2 = work / "gen2.v"
    v3 = work / "gen3.v"
    text1 = v1.read_text()
    text2 = read_back(odin3, lib, v1, v2)
    text3 = read_back(odin3, lib, v2, v3)
    if strip_comments(text2) != strip_comments(text3):
        raise CheckError("roundtrip: generation 3 differs from generation 2 (not a fixpoint)")
    check_defaults(text2, "generation 2")
    one, two = instances(text1), instances(text2)
    if len(one) != len(two):
        raise CheckError(f"roundtrip: {len(one)} instances written, {len(two)} read back")
    for a, b in zip(one, two, strict=True):
        if a != b:
            raise CheckError(f"roundtrip: instance {a[2]} changed: {a} -> {b}")


# --- the BLIF round trip ------------------------------------------------------------------------

SUBCKT_RE = re.compile(r"^\.subckt\s+(\S+)((?:\s+\S+=\S+|\s*\\\n)*)", re.M)


def lib_directions(lib: Path) -> dict[str, dict[str, str]]:
    """``cell -> port -> in|out`` from the statements of an .o3lib file (one per line)."""
    cells: dict[str, dict[str, str]] = {}
    current: dict[str, str] | None = None
    for raw in lib.read_text().splitlines():
        words = raw.split("#", 1)[0].split()
        if len(words) >= 2 and words[0] == "cell":
            current = cells.setdefault(words[1], {})
        elif words[:1] == ["end"]:
            current = None
        elif current is not None and len(words) >= 2 and words[0] in ("in", "out"):
            current[words[1]] = words[0]
    return cells


def with_stubs(text: str, dirs: dict[str, dict[str, str]]) -> str:
    """The BLIF text plus a ``.blackbox`` model for every library cell it instantiates."""
    joined = text.replace("\\\n", " ")
    used: dict[str, dict[str, None]] = {}
    for match in SUBCKT_RE.finditer(joined):
        if match.group(1) in dirs:
            formals = used.setdefault(match.group(1), {})
            for tok in match.group(2).split():
                formals[tok.partition("=")[0]] = None
    out = [text if text.endswith("\n") else text + "\n"]
    for name, formals in sorted(used.items()):
        ports = dirs[name]
        ins = [f for f in formals if ports.get(f.partition("[")[0]) == "in"]
        outs = [f for f in formals if ports.get(f.partition("[")[0]) == "out"]
        out.append(f".model {name}\n")
        out += [f"{d} {' '.join(p)}\n" for d, p in ((".inputs", ins), (".outputs", outs)) if p]
        out.append(".blackbox\n.end\n")
    return "".join(out)


def check_blif(args: argparse.Namespace, text: str, out: Path, work: Path) -> None:
    """Write BLIF, read it back, write again: identical text, netlist-compare identical."""
    first = work / "gen1.blif"
    second = work / "gen2.blif"
    odin3_run(args.odin3, text.replace(f"write_verilog {out}", f"write_blif {first}"))
    odin3_run(
        args.odin3,
        f"read_techlib {args.lib}; read_blif {first}; write_blif {second}",
    )
    if first.read_text() != second.read_text():
        raise CheckError("blif: the second write differs from the first")
    dirs = lib_directions(args.lib)
    stubbed = []
    for path in (first, second):
        copy = path.with_suffix(".stub.blif")
        copy.write_text(with_stubs(path.read_text(), dirs))
        stubbed.append(str(copy))
    proc = run([str(args.compare), *stubbed])
    if proc.returncode != 0:
        first_line = (proc.stdout + proc.stderr).strip().splitlines()[:1]
        raise CheckError(f"blif: netlist-compare exit {proc.returncode}: {first_line}")


# --- the lint -----------------------------------------------------------------------------------


def check_lint(v1: Path, use_verilator: bool = True) -> None:
    proc = run(["iverilog", "-g2005", "-Wall", "-o", "/dev/null", str(v1)])
    notes = [ln for ln in proc.stderr.splitlines() if ln.strip() and not FLOATING.search(ln)]
    if proc.returncode != 0 or notes:
        raise CheckError("lint: iverilog: " + "; ".join(notes[:3] or [f"exit {proc.returncode}"]))
    if use_verilator and shutil.which("verilator"):
        vp = run(
            [
                "verilator",
                "--lint-only",
                "-Wall",
                "-Wno-DECLFILENAME",
                "-Wno-UNUSED",
                "-Wno-UNDRIVEN",
                "-Wno-PINCONNECTEMPTY",
                "-Wno-PINMISSING",
                str(v1),
            ]
        )
        if vp.returncode != 0:
            first = (vp.stderr.strip().splitlines() or ["failed"])[0]
            raise CheckError(f"lint: verilator: {first}")


# --- the driver ---------------------------------------------------------------------------------


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="lpm-check", description=__doc__.split("\n")[0])
    ap.add_argument("micro", type=Path)
    ap.add_argument("--odin3", default="odin3")
    ap.add_argument("--lib", type=Path, default=DEFAULT_LIB)
    ap.add_argument("--script", type=Path, default=DEFAULT_SCRIPT)
    ap.add_argument("--expect", type=Path, default=DEFAULT_EXPECT)
    ap.add_argument("--family", default=None)
    ap.add_argument("--netlist-compare", dest="compare", type=Path, default=DEFAULT_COMPARE)
    ap.add_argument("--keep", type=Path, default=None)
    ap.add_argument("--lint-only", action="store_true")
    return ap.parse_args(argv)


def check_micro(args: argparse.Namespace, work: Path) -> None:
    out = work / "gen1.v"
    text = script_text(args.script, args.lib, args.micro, out, args.family)
    proc = odin3_run(args.odin3, text)
    exp = read_expect(args.expect, args.micro.name)
    params = exp.params
    if args.family:  # the expect rows name the default family
        params = [row for row in params if row[1] != "intended_device_family"]
    written = out.read_text()
    check_counts(written, proc.stderr, exp.counts, params, exp.pins)
    check_defaults(written, "generation 1")
    wrote = f'.intended_device_family("{args.family}")' in written
    if args.family and exp.counts and exp.counts[3] and not wrote:
        raise CheckError(f"counts: --family {args.family}: no altsyncram carries it")
    if exp.roundtrip:
        check_roundtrip(args.odin3, args.lib, out, work)
        check_blif(args, text, out, work)
    if exp.lint:
        check_lint(out)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    if shutil.which("iverilog") is None:
        print("lpm-check: iverilog is not installed", file=sys.stderr)
        return 77
    if not args.micro.is_file():
        print(f"lpm-check: no such file: {args.micro}", file=sys.stderr)
        return 2
    try:
        if args.lint_only:
            check_lint(args.micro)
            return 0
        with tempfile.TemporaryDirectory(prefix="lpm-check.") as tmp:
            work = Path(tmp)
            try:
                check_micro(args, work)
            finally:
                if args.keep is not None:
                    shutil.copytree(work, args.keep, dirs_exist_ok=True)
    except CheckError as exc:
        print(f"lpm-check: {args.micro.name}: {exc}", file=sys.stderr)
        return 2 if exc.usage else 1
    print(f"lpm-check: {args.micro.name}: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

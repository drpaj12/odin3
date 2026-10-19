"""lpm-check: one LPM micro through the script, the Verilog round trip and the lint.

5.1 spec T-1..T-4.

Usage::

    lpm-check [--odin3 P] [--lib L] [--script S] [--expect F] [--family NAME] [--keep DIR]
              [--lint-only] MICRO.v

Legs, in order (the first failure is printed and the exit status is 1):

1. ``counts``: Odin III runs the script (``tests/lpm/lpm.script``) on the micro and writes
   Verilog; the INFO line ``lpmmap: N lpm_add_sub, ...`` must equal the ``counts`` row of the
   expect file, and every ``param`` row must be found on an instance of its type.
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


def read_expect(path: Path, micro: str) -> tuple[list[int] | None, list[tuple[str, str, str]]]:
    counts: list[int] | None = None
    params: list[tuple[str, str, str]] = []
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 4)
        if parts[0] == "counts" and parts[1] == micro:
            counts = [int(n) for n in parts[2:7]] if len(parts) >= 7 else None
            if counts is None:
                counts = [int(n) for n in line.split()[2:]]
        elif parts[0] == "param" and parts[1] == micro:
            params.append((parts[2], parts[3], parts[4]))
    return counts, params


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


def check_counts(
    text: str, info: str, counts: list[int] | None, params: list[tuple[str, str, str]]
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
    one, two = instances(text1), instances(text2)
    if len(one) != len(two):
        raise CheckError(f"roundtrip: {len(one)} instances written, {len(two)} read back")
    for a, b in zip(one, two, strict=True):
        if a != b:
            raise CheckError(f"roundtrip: instance {a[2]} changed: {a} -> {b}")


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
    ap.add_argument("--keep", type=Path, default=None)
    ap.add_argument("--lint-only", action="store_true")
    return ap.parse_args(argv)


def check_micro(args: argparse.Namespace, work: Path) -> None:
    out = work / "gen1.v"
    text = script_text(args.script, args.lib, args.micro, out, args.family)
    proc = odin3_run(args.odin3, text)
    counts, params = read_expect(args.expect, args.micro.name)
    if args.family:  # the expect rows name the default family
        params = [row for row in params if row[1] != "intended_device_family"]
    check_counts(out.read_text(), proc.stderr, counts, params)
    wrote = f'.intended_device_family("{args.family}")' in out.read_text()
    if args.family and counts and counts[3] and not wrote:
        raise CheckError(f"counts: --family {args.family}: no altsyncram carries it")
    check_roundtrip(args.odin3, args.lib, out, work)
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

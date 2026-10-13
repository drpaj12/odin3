"""elab-equiv: 2D's elaboration against Yosys 0.55's on one file (2D spec §14.3, plan Task 21).

Usage::

    elab-equiv --odin3 O --yosys Y --equiv-check E [--top T] [--mem-cap BYTES] [--keep DIR]
               [--allow-version V] [--a-only] [--unmap-enables] FILE

Side A is Odin III's elaboration and side B Yosys's own, both through one Yosys back end, so x is
resolved by the same ``opt`` on both and a difference is 2D's (or a documented divergence):

- A: ``O [--top T] -p "read_verilog --nolatches F; write_rtlil a.il"``, then ``Y -p "read_rtlil
  a.il; hierarchy -top T; proc; flatten; opt; memory -nomap; opt; memory_map; opt -full; techmap;
  opt -fast; abc -g AND,NAND,OR,NOR,XOR,XNOR,MUX; opt_clean; write_blif -gates -undef + unconn
  a.blif"``. Without ``--top`` the script says ``hierarchy -auto-top``.
- B: the same Yosys script with ``read_verilog -sv -nolatches F`` in place of ``read_rtlil a.il``.
  Both sides are always ``--nolatches``: equiv-check cannot model latches (PHASE0 #7).
- ``Y -p "read_rtlil a.il; write_rtlil b.il"`` must exit 0 (write_rtlil's text is valid RTLIL).
- Then ``E a.blif b.blif`` (tools/equiv-check/equiv-check).

``--unmap-enables`` keeps enables out of the flip-flops on both sides (``opt -nodffe -nosdff``
and a ``dffunmap`` before ``techmap``): ``memory_map`` and ``opt_dff`` make enabled flip-flops,
which ``equiv-check`` cannot read as ``.gate`` cells (2D Task 24's memory bridge).

``--a-only`` stops after ``a.blif`` (kept in ``--keep DIR``): the hand-off to 2H (plan point (m)),
which compares side A's netlist with Parmys's goldens. ``--keep DIR`` keeps every file; without it
a temporary directory is used and removed. Every child runs in that directory through
``memcap.run(argv, cap)`` (point (n); ``--mem-cap`` bytes, default 4 GiB). A Yosys whose ``-V``
lacks ``0.55`` is refused unless ``--allow-version V`` names it (the nightly drift job only).

Exit codes: 0 equivalent; 1 NOT equivalent, only when ``equiv-check`` exits 1 *and* prints its own
``equiv-check: NOT equivalent`` verdict line; 2 error: a tool named on the command line that is
not an executable, a child that failed or crashed (any exit but 0 from odin3 or Yosys, an exit
other than 0/1 from ``equiv-check``, an exit 1 without its verdict line such as a Python
traceback or a ``systemd-run`` that cannot find the program), a bad argument; 77 skipped (an ASan
child and no ``systemd-run``: it cannot be capped). A driver that gates on "differs" must test for exit 1 only.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import memcap

TOOL = "elab-equiv"
GIB = 1 << 30
DEFAULT_CAP = 4 * GIB  # plan point (n): 4 GiB per child by default
EXIT_EQUAL, EXIT_DIFFER, EXIT_ERROR, EXIT_SKIP = 0, 1, 2, 77
# equiv-check's own verdict line for exit 1 (tools/equiv-check): without it exit 1 is a crash.
NOT_EQUIVALENT_RE = re.compile(r"^equiv-check: NOT equivalent\b", re.MULTILINE)
TAIL_LINES = 20  # lines of a failing child's output shown

# Yosys's script after the read: hierarchy (the top is chosen by --top or found), proc, then the
# Parmys-style flattening to gates (the one back end both sides share).
BACK_END = (
    "proc; flatten; opt{o}; memory -nomap; opt{o}; memory_map; opt -full{o}; {unmap}techmap; "
    "opt -fast{o}; "
    "abc -g AND,NAND,OR,NOR,XOR,XNOR,MUX; opt_clean; write_blif -gates -undef + unconn {blif}"
)
_ORACLE_RE = re.compile(r"\bYosys 0\.55(?:[^0-9]|$)")


class EquivError(Exception):
    """A usage error or a failing child (exit code 2)."""


@dataclass
class Options:
    odin3: str
    yosys: str
    equiv_check: str
    source: Path
    top: str | None
    cap: int
    keep: Path | None
    allow_version: str | None
    a_only: bool
    unmap: bool = False


def yosys_script(read: str, top: str | None, blif: str, unmap: bool = False) -> str:
    """Yosys's ``-p`` script: the read, the hierarchy, the shared back end writing ``blif``."""
    hierarchy = f"hierarchy -top {top}" if top else "hierarchy -auto-top"
    flags = " -nodffe -nosdff" if unmap else ""
    back_end = BACK_END.format(blif=blif, unmap="dffunmap; " if unmap else "", o=flags)
    return f"{read}; {hierarchy}; {back_end}"


def version_ok(version: str, allow: str | None) -> bool:
    """True for Yosys 0.55, or for a ``-V`` line that names ``allow``."""
    return bool(_ORACLE_RE.search(version)) or (allow is not None and allow in version)


def tail(text: str) -> str:
    lines = text.strip().splitlines()
    return "\n".join(lines[-TAIL_LINES:])


class Bridge:
    """One file through steps A, B, the RTLIL round trip and equiv-check."""

    def __init__(self, opts: Options) -> None:
        self.opts = opts
        self.work = Path()

    def child(self, argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
        """Runs one child in the work directory under the cap; OSError is an error."""
        try:
            return memcap.run(list(argv), self.opts.cap, capture_output=True, cwd=self.work)
        except OSError as exc:
            raise EquivError(f"cannot run {argv[0]}: {exc}") from exc
        except ValueError as exc:
            raise EquivError(f"memcap refused {argv[0]}: {exc}") from exc

    def must(self, what: str, argv: Sequence[str]) -> None:
        """A child that has to exit 0."""
        done = self.child(argv)
        if done.returncode != 0:
            raise EquivError(f"{what} failed (exit {done.returncode}):\n"
                             f"{tail((done.stdout or '') + (done.stderr or ''))}")

    def check_version(self) -> None:
        done = self.child([self.opts.yosys, "-V"])
        version = (done.stdout or "").strip()
        if done.returncode != 0 or not version_ok(version, self.opts.allow_version):
            raise EquivError(f"{self.opts.yosys} is '{version or 'unknown'}', not Yosys 0.55 "
                             "(--allow-version accepts another)")

    def side_a(self) -> None:
        opts = self.opts
        odin3 = [opts.odin3, *(["--top", opts.top] if opts.top else []), "-p",
                 f"read_verilog --nolatches {opts.source}; write_rtlil a.il"]
        self.must("odin3 (side A)", odin3)
        if not (self.work / "a.il").is_file():
            raise EquivError("odin3 (side A) wrote no a.il")
        self.blif("side A", "read_rtlil a.il", "a.blif")

    def side_b(self) -> None:
        read = f"read_verilog -sv -nolatches {self.opts.source}"
        self.blif("side B", read, "b.blif")

    def blif(self, what: str, read: str, name: str) -> None:
        script = yosys_script(read, self.opts.top, name, self.opts.unmap)
        self.must(f"Yosys {what}", [self.opts.yosys, "-p", script])
        if not (self.work / name).is_file():
            raise EquivError(f"Yosys {what} wrote no {name}")

    def round_trip(self) -> None:
        """Yosys reads write_rtlil's text back."""
        self.must("Yosys's read_rtlil of a.il",
                  [self.opts.yosys, "-p", "read_rtlil a.il; write_rtlil b.il"])

    def equivalence(self) -> int:
        done = self.child([self.opts.equiv_check, "a.blif", "b.blif"])
        name = self.opts.source.name
        if done.returncode == 0:
            print(f"{TOOL}: {name}: equivalent")
            return EXIT_EQUAL
        output = tail((done.stdout or "") + (done.stderr or ""))
        if done.returncode == 1 and NOT_EQUIVALENT_RE.search(done.stdout or ""):
            print(f"{TOOL}: {name}: NOT equivalent\n{output}")
            return EXIT_DIFFER
        raise EquivError(f"equiv-check failed (exit {done.returncode}, no verdict line):\n{output}")

    def check_tools(self) -> None:
        """Every tool named on the command line is an executable, else exit 2 before any child."""
        opts = self.opts
        named = [("--odin3", opts.odin3), ("--yosys", opts.yosys)]
        named += [] if opts.a_only else [("--equiv-check", opts.equiv_check)]
        for flag, tool in named:
            if shutil.which(tool) is None:
                raise EquivError(f"{flag} {tool} is not an executable file")

    def run(self) -> int:
        opts = self.opts
        if not opts.source.is_file():
            raise EquivError(f"{opts.source} is not a file")
        temp: str | None = None
        if opts.keep is not None:
            opts.keep.mkdir(parents=True, exist_ok=True)
            self.work = opts.keep.resolve()
        else:
            temp = tempfile.mkdtemp(prefix="elab-equiv-")
            self.work = Path(temp)
        try:
            self.check_tools()
            self.check_version()
            self.side_a()
            if opts.a_only:
                print(f"{TOOL}: {opts.source.name}: a.blif in {self.work}")
                return EXIT_EQUAL
            self.side_b()
            self.round_trip()
            return self.equivalence()
        finally:
            if temp is not None:
                shutil.rmtree(temp, ignore_errors=True)


def exe(name: str) -> str:
    """The executable as an absolute path (the children run in the work directory); a bare name
    stays one, for PATH."""
    return str(Path(name).resolve()) if "/" in name else name


def parse(argv: Sequence[str] | None) -> Options:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary (side A)")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary (side B, back end)")
    ap.add_argument("--equiv-check", required=True, help="tools/equiv-check/equiv-check")
    ap.add_argument("--top", help="the top module (hierarchy -top; else -auto-top)")
    ap.add_argument("--mem-cap", type=int, default=DEFAULT_CAP, metavar="BYTES",
                    help="the memory cap of every child (default 4 GiB)")
    ap.add_argument("--keep", type=Path, metavar="DIR", help="keep the files in DIR")
    ap.add_argument("--allow-version", metavar="V", help="accept a Yosys whose -V names V")
    ap.add_argument("--a-only", action="store_true", help="stop after a.blif (the 2H hand-off)")
    ap.add_argument("--unmap-enables", action="store_true",
                    help="dffunmap before techmap (the memory bridge's enabled flip-flops)")
    ap.add_argument("file", type=Path, help="the Verilog file")
    args = ap.parse_args(argv)
    return Options(exe(args.odin3), exe(args.yosys), exe(args.equiv_check), args.file.resolve(),
                   args.top,
                   args.mem_cap, args.keep, args.allow_version, args.a_only, args.unmap_enables)


def main(argv: Sequence[str] | None = None) -> int:
    opts = parse(argv)
    try:
        return Bridge(opts).run()
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    except EquivError as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())

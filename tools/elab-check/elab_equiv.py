"""elab-equiv: 2D's elaboration against Yosys 0.55's on one file (2D spec §14.3, plan Task 21).

Usage::

    elab-equiv --odin3 O --yosys Y --equiv-check E [--top T] [--mem-cap BYTES] [--keep DIR]
               [--allow-version V] [--a-only] [--unmap-enables] [--yosys-tribuf] [--techlib T] FILE

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

``--yosys-tribuf`` runs ``tribuf -logic`` after ``proc; flatten`` on side B only (after the flatten,
so that a child's output port is an internal net, as 2D sees it). Plain ``read_verilog; proc``
shorts the several drivers of an internal net (every driver's data reaches the net, whatever the
enables say), so a design with internal tri-state buses is not comparable without it. 2D converts
them itself (D3, spec 17 #79: a ``$pmux`` over an x default, as ``tribuf -logic`` does), so side A
needs no flag. ``tribuf -logic`` leaves a bus that reaches an output port a ``$tribuf``, which 2D's
side A keeps too, but ``equiv-check`` cannot read that: use the option on designs whose tri-states
are all internal.

``--a-only`` stops after ``a.blif`` (kept in ``--keep DIR``): the hand-off to 2H (plan point (m)),
which compares side A's netlist with Parmys's goldens. ``--keep DIR`` keeps every file; without it
a temporary directory is used and removed. Every child runs in that directory through
``memcap.run(argv, cap)`` (point (n); ``--mem-cap`` bytes, default 4 GiB). A Yosys whose ``-V``
lacks ``0.55`` is refused unless ``--allow-version V`` names it (the nightly drift job only).

Exit codes: 0 equivalent; 1 NOT equivalent, only when ``equiv-check`` exits 1 *and* prints its own
``equiv-check: NOT equivalent`` verdict line; 2 error: a tool named on the command line that is
not an executable, a child that failed or crashed (any exit but 0 from odin3 or Yosys, an exit
other than 0/1 from ``equiv-check``, an exit 1 without its verdict line such as a Python
traceback or a ``systemd-run`` that cannot find the program), a bad argument; 3 oracle crash
(Task 26: Yosys died of a signal other than the cap's SIGKILL on side B, the Yosys read of the
source, or while odin3 itself rejected the file: Yosys 0.55 segfaults on recursive instantiation,
where 2D reports "recursive instantiation of 'top'"; neither a mismatch nor a 2D failure); 77
skipped (an ASan child and no ``systemd-run``: it cannot be capped). A child killed by its cap is
reported ``out of memory (cap 4G)``. A driver that gates on "differs" tests for exit 1 only.
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
GIB, MIB = 1 << 30, 1 << 20
DEFAULT_CAP = 4 * GIB  # plan point (n): 4 GiB per child by default
EXIT_EQUAL, EXIT_DIFFER, EXIT_ERROR, EXIT_ORACLE_CRASH, EXIT_SKIP = 0, 1, 2, 3, 77
SIGKILL = 9  # how a cgroup's MemoryMax ends a child
CRASH_SIGNALS = (4, 6, 7, 8, 11)  # SIGILL, SIGABRT, SIGBUS, SIGFPE, SIGSEGV
SHELL_SIGNAL = 128  # a shell reports a signal as 128 + n
# equiv-check's own verdict line for exit 1 (tools/equiv-check): without it exit 1 is a crash.
NOT_EQUIVALENT_RE = re.compile(r"^equiv-check: NOT equivalent\b", re.MULTILINE)
TAIL_LINES = 20  # lines of a failing child's output shown

# Yosys's script after the read: hierarchy (the top is chosen by --top or found), proc, then the
# Parmys-style flattening to gates (the one back end both sides share).
BACK_END = (
    "proc; flatten; {tribuf}opt{o}; memory -nomap; opt{o}; memory_map; opt -full{o}; "
    "{unmap}techmap; opt -fast{o}; "
    "abc -g AND,NAND,OR,NOR,XOR,XNOR,MUX; opt_clean; write_blif -gates -undef + unconn {blif}"
)
_ORACLE_RE = re.compile(r"\bYosys 0\.55(?:[^0-9]|$)")


class EquivError(Exception):
    """A usage error or a failing child (exit code 2)."""


class OracleCrash(Exception):
    """Yosys died of a signal (exit code 3): the oracle's bug, not a verdict on 2D."""


def format_cap(cap: int) -> str:
    """A cap as the reports spell it: 4G, 512M, else bytes."""
    if cap % GIB == 0:
        return f"{cap // GIB}G"
    return f"{cap // MIB}M" if cap % MIB == 0 else str(cap)


def signal_of(code: int) -> int | None:
    """The signal that ended a child (a negative status, or a shell's 128 + n), else None."""
    if code < 0:
        return -code
    return code - SHELL_SIGNAL if code > SHELL_SIGNAL else None


def killed_by_cap(code: int) -> bool:
    return signal_of(code) == SIGKILL


def crashed(code: int) -> bool:
    return signal_of(code) in CRASH_SIGNALS


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
    techlib: str | None = None
    yosys_tribuf: bool = False


def wants_directive(source: Path, flag: str) -> bool:
    """True when the file's head holds a ``// elab-check: ... <flag>`` directive, the same in-file
    spelling elab-check honours (a line after line 1, within the first 1024 bytes)."""
    try:
        head = source.read_bytes()[:1024].decode("utf-8", errors="replace")
    except OSError:
        return False
    return any(
        line.startswith("// elab-check:") and flag in line.split()
        for line in head.split("\n")[1:]
    )


def wants_auto_blackbox(source: Path) -> bool:
    """True when the file asks for ``--auto-blackbox`` (side A reads it that way)."""
    return wants_directive(source, "--auto-blackbox")


def wants_yosys_tribuf(source: Path) -> bool:
    """True when the file asks for ``--yosys-tribuf``: its side B runs ``tribuf -logic`` (a file whose
    tri-state buses odin3 turns into logic, 2D-7b review I3)."""
    return wants_directive(source, "--yosys-tribuf")


def yosys_script(read: str, top: str | None, blif: str, unmap: bool = False,
                 tribuf: bool = False) -> str:
    """Yosys's ``-p`` script: the read, the hierarchy, the shared back end writing ``blif``.
    ``tribuf``: ``tribuf -logic`` after ``proc; flatten`` (side B of ``--yosys-tribuf``)."""
    hierarchy = f"hierarchy -top {top}" if top else "hierarchy -auto-top"
    flags = " -nodffe -nosdff" if unmap else ""
    back_end = BACK_END.format(blif=blif, unmap="dffunmap; " if unmap else "", o=flags,
                               tribuf="tribuf -logic; " if tribuf else "")
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

    def must(self, what: str, argv: Sequence[str], oracle: bool = False) -> None:
        """A child that has to exit 0. ``oracle``: it is Yosys reading the source itself, so a
        crash is the oracle's (OracleCrash)."""
        done = self.child(argv)
        code = done.returncode
        if code == 0:
            return
        if oracle and crashed(code):
            raise OracleCrash(f"{what} crashed (signal {signal_of(code)})")
        if killed_by_cap(code):
            raise EquivError(f"{what} failed: out of memory (cap {format_cap(self.opts.cap)})")
        raise EquivError(f"{what} failed (exit {code}):\n"
                         f"{tail((done.stdout or '') + (done.stderr or ''))}")

    def check_version(self) -> None:
        done = self.child([self.opts.yosys, "-V"])
        version = (done.stdout or "").strip()
        if done.returncode != 0 or not version_ok(version, self.opts.allow_version):
            raise EquivError(f"{self.opts.yosys} is '{version or 'unknown'}', not Yosys 0.55 "
                             "(--allow-version accepts another)")

    def side_a(self) -> None:
        opts = self.opts
        flags = "--nolatches --auto-blackbox" if wants_auto_blackbox(opts.source) else "--nolatches"
        lib = f"read_techlib {opts.techlib}; " if opts.techlib else ""
        odin3 = [opts.odin3, *(["--top", opts.top] if opts.top else []), "-p",
                 f"{lib}read_verilog {flags} {opts.source}; write_rtlil a.il"]
        try:
            self.must("odin3 (side A)", odin3)
        except EquivError as exc:
            self.probe_oracle(exc)
            raise
        if not (self.work / "a.il").is_file():
            raise EquivError("odin3 (side A) wrote no a.il")
        self.blif("side A", "read_rtlil a.il", "a.blif")

    def side_b(self) -> None:
        read = f"read_verilog -sv -nolatches {self.opts.source}"
        self.blif("side B", read, "b.blif", oracle=True, tribuf=self.opts.yosys_tribuf or wants_yosys_tribuf(self.opts.source))

    def probe_oracle(self, rejected: EquivError) -> None:
        """odin3 rejected the file: if Yosys dies of a signal on the same source, that is an
        oracle crash (recursive instantiation, Task 26), else the rejection stands."""
        try:
            self.side_b()
        except OracleCrash as crash:
            lines = str(rejected).strip().splitlines()
            why = next((ln for ln in lines if "error" in ln), lines[-1] if lines else "no output")
            raise OracleCrash(f"{crash}; odin3 rejected the file: {why}") from crash
        except EquivError:
            pass

    def blif(self, what: str, read: str, name: str, oracle: bool = False,
             tribuf: bool = False) -> None:
        script = yosys_script(read, self.opts.top, name, self.opts.unmap, tribuf)
        self.must(f"Yosys {what}", [self.opts.yosys, "-p", script], oracle)
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
    ap.add_argument("--yosys-tribuf", action="store_true",
                    help="side B runs tribuf -logic after proc; flatten (internal tri-state buses)")
    ap.add_argument("--techlib", metavar="T", help="a .o3lib read first on side A (hard blocks)")
    ap.add_argument("file", type=Path, help="the Verilog file")
    args = ap.parse_args(argv)
    return Options(exe(args.odin3), exe(args.yosys), exe(args.equiv_check), args.file.resolve(),
                   args.top,
                   args.mem_cap, args.keep, args.allow_version, args.a_only, args.unmap_enables,
                   str(Path(args.techlib).resolve()) if args.techlib else None,
                   args.yosys_tribuf)


def main(argv: Sequence[str] | None = None) -> int:
    opts = parse(argv)
    try:
        return Bridge(opts).run()
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    except OracleCrash as exc:
        print(f"{TOOL}: oracle crash: {exc}")
        return EXIT_ORACLE_CRASH
    except EquivError as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())

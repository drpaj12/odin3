"""elab-equiv: 2D's elaboration against Yosys 0.55's on one file (2D spec §14.3, plan Task 21).

Usage::

    elab-equiv --odin3 O --yosys Y --equiv-check E [--top T] [--mem-cap BYTES] [--keep DIR]
               [--allow-version V] [--a-only] [--unmap-enables] [--pass elab|proc] [--no-trio]
               [--async2sync] FILE

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

``--async2sync`` runs Yosys's ``async2sync`` before ``techmap`` on both sides (2E Task 11):
``equiv-check`` reads no ``.gate`` flip-flop with an asynchronous reset, set or load, so the rows
that have one are checked after the same rewrite of both sides into a ``$dff`` and logic.

``--pass proc`` (2E spec 18.5, point (r)) moves side A to the Phase 2 script up to ``proc``:
``O [--top T] -p "read_verilog --nolatches F; opt_expr; opt_clean; opt -nodffe -nosdff; proc;
write_rtlil a.il"`` (2E's ``proc`` is run, so ``a.il`` holds no process and Yosys's ``proc`` in the
back end is a no-op). ``--no-trio`` drops the three ``opt`` passes (until 2F-5 has merged ``opt``).
Side B and the back end are unchanged. The default ``--pass elab`` is 2D's bridge, which ignores
``--no-trio``. It is an equivalence oracle only (the exit splice is 2F's S-P, 2E 18.5).

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

# 2E's side A, up to proc, after the pre-proc passes of 2F's mode P (2E spec 18.5).
PRE_PROC_TRIO = "opt_expr; opt_clean; opt -nodffe -nosdff; "

# Yosys's script after the read: hierarchy (the top is chosen by --top or found), proc, then the
# Parmys-style flattening to gates (the one back end both sides share).
BACK_END = (
    "proc; flatten; opt{o}; memory -nomap; opt{o}; memory_map; opt -full{o}; {a2s}{unmap}techmap; "
    "opt -fast{o}; "
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
    pass_name: str = "elab"
    async2sync: bool = False
    no_trio: bool = False


def wants_auto_blackbox(source: Path) -> bool:
    """True when the file's head holds the ``// elab-check: ... --auto-blackbox`` directive, the
    same in-file spelling elab-check honours (a line after line 1, within the first 1024 bytes)."""
    try:
        head = source.read_bytes()[:1024].decode("utf-8", errors="replace")
    except OSError:
        return False
    return any(
        line.startswith("// elab-check:") and "--auto-blackbox" in line
        for line in head.split("\n")[1:]
    )


def yosys_script(read: str, top: str | None, blif: str, unmap: bool = False,
                 async2sync: bool = False) -> str:
    """Yosys's ``-p`` script: the read, the hierarchy, the shared back end writing ``blif``."""
    hierarchy = f"hierarchy -top {top}" if top else "hierarchy -auto-top"
    flags = " -nodffe -nosdff" if unmap else ""
    back_end = BACK_END.format(blif=blif, unmap="dffunmap; " if unmap else "", o=flags,
                               a2s="async2sync; " if async2sync else "")
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
        if opts.pass_name == "proc":
            trio = "" if opts.no_trio else PRE_PROC_TRIO
            script = f"read_verilog {flags} {opts.source}; {trio}proc; write_rtlil a.il"
        else:
            script = f"read_verilog {flags} {opts.source}; write_rtlil a.il"
        odin3 = [opts.odin3, *(["--top", opts.top] if opts.top else []), "-p", script]
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
        self.blif("side B", read, "b.blif", oracle=True)

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

    def blif(self, what: str, read: str, name: str, oracle: bool = False) -> None:
        script = yosys_script(read, self.opts.top, name, self.opts.unmap, self.opts.async2sync)
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
    ap.add_argument("--async2sync", action="store_true",
                    help="async2sync before techmap on both sides (equiv-check reads no "
                         "asynchronous flip-flop)")
    ap.add_argument("--pass", dest="pass_name", choices=("elab", "proc"), default="elab",
                    help="side A stops after elaboration (elab, default) or after proc")
    ap.add_argument("--no-trio", action="store_true",
                    help="--pass proc: no opt_expr/opt_clean/opt before proc (until 2F-5)")
    ap.add_argument("file", type=Path, help="the Verilog file")
    args = ap.parse_args(argv)
    return Options(exe(args.odin3), exe(args.yosys), exe(args.equiv_check), args.file.resolve(),
                   args.top,
                   args.mem_cap, args.keep, args.allow_version, args.a_only, args.unmap_enables,
                   args.pass_name, args.async2sync, args.no_trio)


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

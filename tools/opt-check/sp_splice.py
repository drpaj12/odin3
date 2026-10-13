"""sp-splice: the 2H splice S-P of 2F spec section 3 (OP-3; plan Task 15, point (i)).

Usage::

    sp-splice --odin3 O --yosys Y --arch A --top T [--config C] [--keep DIR] [--primitives P]
              [--mem-cap BYTES] [--allow-version V] [--scc-precheck]
              [--odin3-pre CMD]... [--no-primitives] FILE...

Phase 2 has no ``fsm``, ``wreduce``, ``memory``, ``flatten``, ``techmap`` or hard-block mapping,
so the exit comparison runs Parmys's script in two halves, verbatim from ``synthesis.tcl`` apart
from point (i)'s Odin III spellings (``--nolatches``, ``--nomem2reg``, ``--top``):

- Odin III: ``O -p "read_verilog --nomem2reg P; read_verilog --nolatches FILE...; hierarchy --top
  T; opt_expr; opt_clean; opt -nodffe -nosdff; proc; write_rtlil a.il"``. ``P`` is Parmys's
  ``vtr_primitives.v`` (``--primitives``, else under ``$VTR_ROOT``).
- Yosys 0.55, from ``plugin -i parmys`` to ``write_blif -true + vcc -false + gnd -undef + unconn
  -blackbox out.blif``, reading ``a.il``. It is one line in ``yosys.tcl`` after
  synthesis.tcl's own first line, ``yosys -import``, run as ``Y -c yosys.tcl``: the ``yosys
  -import`` inside the half exists only in Yosys's Tcl mode.

``--top`` stays on the Odin III side: 2D elaborates only the modules under the top, so Yosys's
``hierarchy -check -auto-top -purge_lib`` then picks the same one (spec section 3).
``--odin3-pre CMD`` (repeatable) puts a command at the front of the Odin III half, e.g.
``read_techlib lib/vtr.o3lib`` (the arch's hard blocks); ``--no-primitives`` leaves the
``vtr_primitives.v`` read out. With main today a read of that file alone stops at 2D's
``ambiguous_top``, and the hard blocks come from the tech library instead (agent default; the
spelling of that read is 2H's to settle).
``--scc-precheck`` first runs the Parmys precondition on the source in Yosys (``read_verilog
-nomem2reg P; read_verilog -sv -nolatches FILE...; scc -select; select -assert-none %``): a
combinational loop is a Parmys rejection and ends the run with exit 2.

Every child runs in the work directory through ``memcap.run`` (``--mem-cap`` bytes, default 4 GiB).
A Yosys whose ``-V`` lacks ``0.55`` is refused unless ``--allow-version V`` names it. The work
directory is ``--keep DIR``, else a fresh temporary directory that is left in place: the path of
``out.blif`` is printed on stdout.

Exit codes: 0 success; 2 a child failed, a bad argument or a refused Yosys; 77 skipped (an ASan
child and no ``systemd-run``: it cannot be capped).
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType

TOOL = "sp-splice"
HERE = Path(__file__).resolve().parent
MEMCAP_PY = HERE.parent / "elab-check" / "memcap.py"
GIB = 1 << 30
DEFAULT_CAP = 4 * GIB
EXIT_OK, EXIT_FAIL, EXIT_SKIP = 0, 2, 77
YOSYS_TCL = "yosys.tcl"  # the Yosys half as a Tcl script: `yosys -import` exists only in Tcl mode
TCL_PROLOGUE = "yosys -import"  # as synthesis.tcl's first line: makes `plugin` a Tcl command
TAIL_LINES = 20
_ORACLE_RE = re.compile(r"\bYosys 0\.55(?:[^0-9]|$)")
_PLAIN_RE = re.compile(r"[^\s;#\"]+")
PRIMITIVES_UNDER_VTR = (
    "build/share/yosys/parmys/vtr_primitives.v",
    "parmys/parmys-plugin/techlibs/vtr_primitives.v",
)

# Odin III half: Parmys lines 1-4 (opt_expr; opt_clean; opt -nodffe -nosdff; proc), then the
# hand-over.
ODIN3_HALF = (
    "read_verilog --nomem2reg {primitives}",
    "read_verilog --nolatches {files}",
    "hierarchy --top {top}",
    "opt_expr",
    "opt_clean",
    "opt -nodffe -nosdff",
    "proc",
    "write_rtlil a.il",
)

# Yosys 0.55 half: from `plugin -i parmys` (line 5 on) to the BLIF, as spec section 3 prints it.
YOSYS_HALF = (
    "plugin -i parmys",
    "yosys -import",
    "parmys_arch -a {arch}",
    "read_rtlil a.il",
    "setattr -mod -set keep_hierarchy 1 single_port_ram",
    "setattr -mod -set keep_hierarchy 1 dual_port_ram",
    "hierarchy -check -auto-top -purge_lib",
    "fsm",
    "opt",
    "wreduce",
    "peepopt",
    "opt_clean",
    "share",
    "opt -full",
    "memory -nomap",
    "flatten",
    "opt -full",
    "techmap -map +/parmys/adff2dff.v",
    "techmap -map +/parmys/adffe2dff.v",
    "techmap -map +/parmys/aldff2dff.v",
    "techmap -map +/parmys/aldffe2dff.v",
    "opt -full",
    "parmys -a {arch} -nopass{config}",
    "opt -full",
    "techmap",
    "opt -fast",
    "dffunmap",
    "opt -fast -noff",
    "stat",
    "hierarchy -check -auto-top -purge_lib",
    "write_blif -true + vcc -false + gnd -undef + unconn -blackbox out.blif",
)

# The precondition on the source (spec section 3, lines 2a/2b), Yosys 0.55 spellings.
SCC_PRECHECK = (
    "read_verilog -nomem2reg {primitives}",
    "read_verilog -sv -nolatches {files}",
    "scc -select",
    "select -assert-none %",
)


class SpliceError(Exception):
    """A usage error or a failing child (exit code 2)."""


@dataclass(frozen=True)
class Options:
    odin3: str
    yosys: str
    arch: str
    top: str
    config: str | None
    keep: Path | None
    primitives: str | None
    cap: int
    allow_version: str | None
    scc_precheck: bool
    pre: tuple[str, ...]
    no_primitives: bool
    files: tuple[Path, ...]


def quote(text: str) -> str:
    """A word of an Odin III script: bare when it has no blank, ';', '#' or '"', else quoted."""
    if _PLAIN_RE.fullmatch(text):
        return text
    if '"' in text:
        raise SpliceError(f"cannot quote {text!r} in an Odin III script")
    return f'"{text}"'


def _fill(commands: Sequence[str], **fields: str) -> str:
    return "; ".join(command.format(**fields) for command in commands)


def odin3_script(primitives: str | None, files: Sequence[str], top: str,
                 pre: Sequence[str] = ()) -> str:
    """The Odin III half, one ``-p`` script: ``pre`` first, the ``vtr_primitives.v`` read unless
    ``primitives`` is None."""
    commands = ODIN3_HALF if primitives is not None else ODIN3_HALF[1:]
    fields = {"primitives": quote(primitives or ""), "top": top,
              "files": " ".join(quote(name) for name in files)}
    return "; ".join([*pre, _fill(commands, **fields)])


def yosys_script(arch: str, config: str | None) -> str:
    """The Yosys 0.55 half, one ``-p`` script."""
    return _fill(YOSYS_HALF, arch=arch, config=f" -c {config}" if config else "")


def precheck_script(primitives: str, files: Sequence[str]) -> str:
    """The scc precondition on the source."""
    return _fill(SCC_PRECHECK, primitives=quote(primitives),
                 files=" ".join(quote(name) for name in files))


def version_ok(version: str, allow: str | None) -> bool:
    """True for Yosys 0.55, or for a ``-V`` line that names ``allow``."""
    return bool(_ORACLE_RE.search(version)) or (allow is not None and allow in version)


def exe(name: str) -> str:
    """The executable as an absolute path (the children run in the work directory); a bare name
    stays one, for PATH."""
    return str(Path(name).resolve()) if "/" in name else name


def absolute(name: str | None) -> str | None:
    """A path argument made absolute when it names an existing file (the children run in the work
    directory); anything else is kept as given."""
    return str(Path(name).resolve()) if name and Path(name).exists() else name


def tail(text: str) -> str:
    lines = text.strip().splitlines()
    return "\n".join(lines[-TAIL_LINES:])


def find_primitives(vtr_root: str | None) -> str:
    """Parmys's vtr_primitives.v under ``$VTR_ROOT``, else a SpliceError."""
    if vtr_root:
        for rel in PRIMITIVES_UNDER_VTR:
            candidate = Path(vtr_root) / rel
            if candidate.is_file():
                return str(candidate)
    raise SpliceError("vtr_primitives.v not found: pass --primitives or set VTR_ROOT")


def load_memcap() -> ModuleType:
    """tools/elab-check/memcap.py, as already imported when a test put its directory on sys.path."""
    loaded = sys.modules.get("memcap")
    if loaded is not None:
        return loaded
    spec = importlib.util.spec_from_file_location("memcap", MEMCAP_PY)
    if spec is None or spec.loader is None:
        raise SpliceError(f"cannot load {MEMCAP_PY}")
    module = importlib.util.module_from_spec(spec)
    sys.modules["memcap"] = module
    spec.loader.exec_module(module)
    return module


class Splice:
    """One design through the two halves."""

    def __init__(self, opts: Options, memcap: ModuleType) -> None:
        self.opts = opts
        self.memcap = memcap
        self.work = Path()

    def child(self, argv: Sequence[str]) -> subprocess.CompletedProcess[str]:
        try:
            done: subprocess.CompletedProcess[str] = self.memcap.run(
                list(argv), self.opts.cap, capture_output=True, cwd=self.work)
            return done
        except OSError as exc:
            raise SpliceError(f"cannot run {argv[0]}: {exc}") from exc
        except ValueError as exc:
            raise SpliceError(f"memcap refused {argv[0]}: {exc}") from exc

    def must(self, what: str, argv: Sequence[str]) -> None:
        done = self.child(argv)
        if done.returncode == 0:
            return
        output = tail((done.stdout or "") + (done.stderr or ""))
        raise SpliceError(f"{what} failed (exit {done.returncode}):\n{output}")

    def check_version(self) -> None:
        done = self.child([self.opts.yosys, "-V"])
        version = (done.stdout or "").strip()
        if done.returncode != 0 or not version_ok(version, self.opts.allow_version):
            raise SpliceError(f"{self.opts.yosys} is '{version or 'unknown'}', not Yosys 0.55 "
                              "(--allow-version accepts another)")

    def run(self) -> Path:
        opts = self.opts
        primitives = opts.primitives
        files = [str(path) for path in opts.files]
        if opts.keep is not None:
            opts.keep.mkdir(parents=True, exist_ok=True)
            self.work = opts.keep.resolve()
        else:
            self.work = Path(tempfile.mkdtemp(prefix="sp-splice-"))
        self.check_version()
        if opts.scc_precheck:
            self.must("the scc precondition (a Parmys rejection)",
                      [opts.yosys, "-p", precheck_script(primitives or "", files)])
        self.must("odin3 (the Odin III half)",
                  # --top goes to the CLI: read_verilog elaborates at once, so `hierarchy --top`
                  # in the script comes too late for a file with several roots.
                  [opts.odin3, "--top", opts.top, "-p",
                   odin3_script(None if opts.no_primitives else primitives, files, opts.top,
                                opts.pre)])
        if not (self.work / "a.il").is_file():
            raise SpliceError("odin3 (the Odin III half) wrote no a.il")
        tcl = self.work / YOSYS_TCL
        script = yosys_script(opts.arch, opts.config)
        tcl.write_text(f"{TCL_PROLOGUE}\n{script}\n", encoding="utf-8")
        self.must("Yosys (the Yosys half)", [opts.yosys, "-c", YOSYS_TCL])
        out = self.work / "out.blif"
        if not out.is_file():
            raise SpliceError("Yosys (the Yosys half) wrote no out.blif")
        return out


def parse(argv: Sequence[str] | None, vtr_root: str | None = None) -> Options:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--yosys", required=True, help="the Yosys 0.55 binary")
    ap.add_argument("--arch", required=True, help="the architecture file (parmys -a)")
    ap.add_argument("--top", required=True, help="the top module (the golden's first .model)")
    ap.add_argument("--config", help="Parmys's config file (parmys -c)")
    ap.add_argument("--keep", type=Path, metavar="DIR", help="the work directory (kept)")
    ap.add_argument("--primitives", help="Parmys's vtr_primitives.v (default under $VTR_ROOT)")
    ap.add_argument("--mem-cap", type=int, default=DEFAULT_CAP, metavar="BYTES",
                    help="the memory cap of every child (default 4 GiB)")
    ap.add_argument("--allow-version", metavar="V", help="accept a Yosys whose -V names V")
    ap.add_argument("--scc-precheck", action="store_true",
                    help="first run Parmys's scc precondition on the source")
    ap.add_argument("--odin3-pre", action="append", default=[], metavar="CMD",
                    help="a command at the front of the Odin III half (repeatable)")
    ap.add_argument("--no-primitives", action="store_true",
                    help="leave the vtr_primitives.v read out of the Odin III half")
    ap.add_argument("files", nargs="+", type=Path, help="the Verilog files")
    args = ap.parse_args(argv)
    primitives = args.primitives
    if primitives is None and (args.scc_precheck or not args.no_primitives):
        primitives = find_primitives(vtr_root)
    return Options(
        odin3=exe(args.odin3), yosys=exe(args.yosys), arch=absolute(args.arch) or args.arch,
        top=args.top, config=absolute(args.config), keep=args.keep,
        primitives=absolute(primitives), cap=args.mem_cap, allow_version=args.allow_version,
        scc_precheck=args.scc_precheck, pre=tuple(args.odin3_pre),
        no_primitives=args.no_primitives, files=tuple(path.resolve() for path in args.files))


def main(argv: Sequence[str] | None = None) -> int:
    try:
        opts = parse(argv, os.environ.get("VTR_ROOT"))
        memcap = load_memcap()
        try:
            out = Splice(opts, memcap).run()
        except memcap.CapUnavailable as exc:
            print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
            return EXIT_SKIP
    except SpliceError as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_FAIL
    print(out)
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())

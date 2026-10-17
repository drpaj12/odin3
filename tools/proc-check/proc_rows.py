"""proc-rows: the three CTests over tests/proc/rows (2E spec 18.2, plan Task 11).

Usage::

    proc-rows goldens --odin3 O [--rows DIR]
    proc-rows diff    --odin3 O --yosys Y [--rows DIR]
    proc-rows bridge  --odin3 O --yosys Y --equiv-check E [--rows DIR]

Each lowering run writes its RTLIL to standard output (``write_rtlil -``).

``rows.txt`` lists ``<slug> comb=<0|1> err=<0|1>``; each row is ``<slug>.v`` in the rows directory
(the default is ``tests/proc/rows`` of this checkout). Rows run smallest file first. Every command
runs with the rows directory as its working directory, so a diagnostic names ``<slug>.v``.

- ``goldens`` (CTest ``proc_rows``): for each non-error row ``O -p "read_verilog F; proc --noopt;
  write_rtlil <file>"`` writes exactly ``<slug>.il``, and with ``--nolatches`` (``comb=1`` rows)
  exactly ``<slug>.nolatches.il``. An error row exits 1 with ``pass proc: failed:
  ODIN3_ERR_PARSE`` and its error lines (the ``odin3: error: `` prefix removed from each line of
  the form ``F:L:C: error: ...``) equal ``<slug>.err``. ``case_default``'s golden holds exactly
  one ``$pmux`` of ``WIDTH`` 8 (2D Task 22's chunk test carried end to end, point (g)).
- ``diff`` (CTest ``proc_diff_rows``, Yosys 0.55): ``proc-diff`` per non-error row in both latch
  modes (``--nolatches`` and the default) and in both input modes: ``--no-trio``, and the trio
  once ``opt`` exists (``O -p "read_verilog F; opt -nodffe -nosdff"`` runs). ``diff_expect.txt``
  names the expected differences. A failing row is rerun with ``--bisect`` and the result is
  printed (18.4).
- ``bridge`` (CTest ``proc_bridge_rows``, Yosys 0.55): ``elab-equiv --pass proc --async2sync
  --unmap-enables --top t`` per non-error row, ``--no-trio`` until ``opt`` exists, expecting exit
  0. A row in ``bridge_expect.txt`` (``slug|text|reason``: what the oracle cannot check) must exit
  4 (unproven; 2 before elab-equiv split the two) with ``text`` in its message, so a row that
  starts to pass is noticed.

Exit 0 when every check holds, 1 when one fails (all are reported), 2 on a usage or tool error;
77 (skip) when a child needs a cap that cannot be applied.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType

import proc_diff

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_ROWS = ROOT / "tests" / "proc" / "rows"
TOOL = "proc-rows"
GIB = 1 << 30
CAP = 3 * GIB
EXIT_OK, EXIT_FAIL, EXIT_ERROR, EXIT_UNPROVEN, EXIT_SKIP = 0, 1, 2, 4, 77
ERR_STATUS = "pass proc: failed: ODIN3_ERR_PARSE"
ERROR_LINE_RE = re.compile(r"^odin3: error: (\S+\.v:\d+:\d+: error: .*)$")
ROW_RE = re.compile(r"^(\w+) comb=([01]) err=([01])$")
PMUX_ROW = "case_default"
PMUX_WIDTH = 8


class RowsError(Exception):
    """A usage error or a broken tool (exit 2)."""


@dataclass(frozen=True)
class Row:
    slug: str
    comb: bool
    err: bool


_MEMCAP: list[ModuleType] = []


def load_memcap() -> ModuleType:
    """2D's ``memcap`` module (tools/elab-check), loaded once on first use (shared loader)."""
    if not _MEMCAP:
        module = proc_diff.load_module("memcap", proc_diff.MEMCAP_PY)
        if module is None:
            raise RowsError("tools/elab-check/memcap.py is missing (2D-6)")
        _MEMCAP.append(module)
    return _MEMCAP[0]


def parse_rows(text: str) -> list[Row]:
    rows: list[Row] = []
    for number, line in enumerate(text.splitlines(), 1):
        match = ROW_RE.match(line)
        if match is None:
            raise RowsError(f"rows.txt:{number}: not '<slug> comb=<0|1> err=<0|1>': {line!r}")
        rows.append(Row(match[1], match[2] == "1", match[3] == "1"))
    return rows


def load_rows(rows_dir: Path) -> list[Row]:
    """The rows of ``rows.txt``, smallest ``.v`` first (ties by slug)."""
    try:
        rows = parse_rows((rows_dir / "rows.txt").read_text(encoding="utf-8"))
        return sorted(rows, key=lambda r: ((rows_dir / f"{r.slug}.v").stat().st_size, r.slug))
    except OSError as exc:
        raise RowsError(f"cannot read the rows: {exc}") from exc


def error_lines(stderr: str) -> str:
    """The located error diagnostics of a run, one per line, without ``odin3: error: ``."""
    kept = [m[1] for line in stderr.splitlines() if (m := ERROR_LINE_RE.match(line))]
    return "".join(f"{line}\n" for line in kept)


def pmux_widths(rtlil: str) -> list[int]:
    """The ``WIDTH`` of each ``$pmux`` cell of an RTLIL text."""
    widths: list[int] = []
    lines = rtlil.splitlines()
    for index, line in enumerate(lines):
        if line.strip().startswith("cell $pmux "):
            for param in lines[index + 1:]:
                if param.strip() == "end":
                    break
                if param.strip().startswith("parameter \\WIDTH "):
                    widths.append(int(param.split()[-1]))
    return widths


def parse_bridge_expect(text: str) -> dict[str, tuple[str, str]]:
    """``slug|text|reason`` lines (``#`` comments and blank lines allowed)."""
    found: dict[str, tuple[str, str]] = {}
    for number, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("|", 2)
        if len(parts) != 3 or not all(p.strip() for p in parts):
            raise RowsError(f"bridge_expect.txt:{number}: not 'slug|text|reason': {line!r}")
        found[parts[0].strip()] = (parts[1].strip(), parts[2].strip())
    return found


class Context:
    """The tools and the rows directory; every child goes through ``memcap.run``."""

    def __init__(self, rows_dir: Path, odin3: str, yosys: str = "", equiv: str = "") -> None:
        self.rows_dir = rows_dir
        self.odin3, self.yosys, self.equiv = odin3, yosys, equiv
        self.failures: list[str] = []

    def run(self, argv: Sequence[str]) -> tuple[int, str, str]:
        memcap = load_memcap()
        try:
            done = memcap.run(list(argv), CAP, capture_output=True, cwd=self.rows_dir)
        except OSError as exc:
            raise RowsError(f"cannot run {argv[0]}: {exc}") from exc
        except ValueError as exc:
            raise RowsError(f"memcap refused {argv[0]}: {exc}") from exc
        return done.returncode, done.stdout or "", done.stderr or ""

    def odin3_script(self, script: str) -> tuple[int, str, str]:
        return self.run([self.odin3, "-p", script])

    def fail(self, message: str) -> None:
        self.failures.append(message)
        print(f"{TOOL}: FAIL {message}")

    def read(self, name: str) -> str:
        """A file of the rows directory; a missing one is a failure (empty text), not an abort."""
        try:
            return (self.rows_dir / name).read_text(encoding="utf-8")
        except OSError as exc:
            self.fail(f"cannot read {name}: {exc.strerror}")
            return ""

    def trio_available(self) -> bool:
        """True once ``opt`` exists (2F-5): ``read_verilog F; opt -nodffe -nosdff`` (the trio's
        own flags) runs. A skipped trio is announced, so it cannot stay off unseen."""
        first = next(r for r in load_rows(self.rows_dir) if not r.err)
        code, _, err = self.odin3_script(f"read_verilog {first.slug}.v; opt -nodffe -nosdff")
        if code != 0:
            reason = (err.strip().splitlines() or ["no output"])[-1]
            print(f"{TOOL}: trio: skipped (opt -nodffe -nosdff exit {code}: {reason})")
        return code == 0


def check_lowering(ctx: Context, row: Row, nolatches: bool) -> None:
    suffix = ".nolatches.il" if nolatches else ".il"
    flag = "--nolatches " if nolatches else ""
    code, out, err = ctx.odin3_script(f"read_verilog {flag}{row.slug}.v; proc --noopt; "
                                      "write_rtlil -")
    name = f"{row.slug}{suffix}"
    if code != 0:
        ctx.fail(f"{name}: odin3 exited {code}: {err.strip().splitlines()[-1:]}")
    elif out != ctx.read(name):
        ctx.fail(f"{name}: odin3's write_rtlil differs from the golden")


def check_error_row(ctx: Context, row: Row) -> None:
    code, _, err = ctx.odin3_script(f"read_verilog {row.slug}.v; proc --noopt; write_rtlil -")
    if code == 0 or ERR_STATUS not in err:
        ctx.fail(f"{row.slug}: expected exit != 0 with '{ERR_STATUS}', got exit {code}")
    got, want = error_lines(err), ctx.read(f"{row.slug}.err")
    if got != want:
        ctx.fail(f"{row.slug}.err: got {got!r}, want {want!r}")


def check_pmux(ctx: Context) -> None:
    widths = pmux_widths(ctx.read(f"{PMUX_ROW}.il"))
    if widths != [PMUX_WIDTH]:
        ctx.fail(f"{PMUX_ROW}.il: $pmux WIDTHs {widths}, want exactly [{PMUX_WIDTH}]")


def cmd_goldens(ctx: Context) -> None:
    for row in load_rows(ctx.rows_dir):
        if row.err:
            check_error_row(ctx, row)
            continue
        check_lowering(ctx, row, False)
        if row.comb:
            check_lowering(ctx, row, True)
    check_pmux(ctx)


def proc_diff_argv(ctx: Context, row: Row, nolatches: bool, trio: bool) -> list[str]:
    argv = [str(HERE / "proc-diff"), "--odin3", ctx.odin3, "--yosys", ctx.yosys,
            "--expect-fail", str(ctx.rows_dir.parent / "diff_expect.txt")]
    argv += ["--nolatches"] if nolatches else []
    argv += [] if trio else ["--no-trio"]
    return [*argv, f"{row.slug}.v"]


def cmd_diff(ctx: Context) -> None:
    modes = [False, True] if ctx.trio_available() else [False]
    for row in (r for r in load_rows(ctx.rows_dir) if not r.err):
        for trio in modes:
            for nolatches in (True, False):
                argv = proc_diff_argv(ctx, row, nolatches, trio)
                code, out, err = ctx.run(argv)
                if code == 0:
                    continue
                label = f"{row.slug} nolatches={nolatches} trio={trio}"
                ctx.fail(f"{label}: proc-diff exit {code}: {(out + err).strip()[-400:]}")
                _, bis, bis_err = ctx.run([*argv[:-1], "--bisect", argv[-1]])
                print(f"{TOOL}: bisect {label}:\n{(bis + bis_err).strip()}")


def bridge_argv(ctx: Context, row: Row, trio: bool) -> list[str]:
    argv = [str(ROOT / "tools" / "elab-check" / "elab-equiv"), "--odin3", ctx.odin3,
            "--yosys", ctx.yosys, "--equiv-check", ctx.equiv, "--pass", "proc",
            "--async2sync", "--unmap-enables", "--top", "t"]
    return [*argv, *([] if trio else ["--no-trio"]), f"{row.slug}.v"]


def cmd_bridge(ctx: Context) -> None:
    expect = parse_bridge_expect(ctx.read("../bridge_expect.txt"))
    trio = ctx.trio_available()
    names = {r.slug for r in load_rows(ctx.rows_dir)}
    for slug in sorted(set(expect) - names):
        ctx.fail(f"bridge_expect.txt names '{slug}', which is not a row")
    for row in (r for r in load_rows(ctx.rows_dir) if not r.err):
        code, out, err = ctx.run(bridge_argv(ctx, row, trio))
        text = out + err
        if row.slug in expect:
            needle, _ = expect[row.slug]
            if code not in (EXIT_ERROR, EXIT_UNPROVEN) or needle not in text:
                ctx.fail(f"{row.slug}: expected exit 4 with '{needle}', got exit {code}")
        elif code == EXIT_SKIP:
            raise load_memcap().CapUnavailable(text.strip())
        elif code != 0:
            ctx.fail(f"{row.slug}: elab-equiv exit {code}: {text.strip()[-400:]}")


COMMANDS: dict[str, Callable[[Context], None]] = {
    "goldens": cmd_goldens, "diff": cmd_diff, "bridge": cmd_bridge,
}


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=sorted(COMMANDS))
    ap.add_argument("--odin3", required=True)
    ap.add_argument("--yosys", default="")
    ap.add_argument("--equiv-check", default="")
    ap.add_argument("--rows", type=Path, default=DEFAULT_ROWS)
    args = ap.parse_args(argv)
    needs = {"goldens": [], "diff": ["yosys"], "bridge": ["yosys", "equiv_check"]}
    missing = [f"--{n.replace('_', '-')}" for n in needs[args.command] if not getattr(args, n)]
    if missing:
        ap.error(f"{args.command} needs {' '.join(missing)}")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    ctx = Context(args.rows.resolve(), str(Path(args.odin3).resolve()),
                  str(Path(args.yosys).resolve()) if args.yosys else "",
                  str(Path(args.equiv_check).resolve()) if args.equiv_check else "")
    try:
        COMMANDS[args.command](ctx)
    except RowsError as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR
    except load_memcap().CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    if ctx.failures:
        print(f"{TOOL}: {len(ctx.failures)} failure(s)")
        return EXIT_FAIL
    print(f"{TOOL}: {args.command}: all rows ok")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())

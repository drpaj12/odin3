"""rescan-compare: `opt` against `opt --opt-full-rescan` on a corpus, byte for byte (plan Task 15;
Task 13 review M7).

Usage::

    rescan-compare --odin3 O [--mem-cap C] [--work DIR] [--tops F] [--golden-root D]
                   [--techlib L] [--root R] [--script S] <dir or file>...

A Release build has no dirty-set guard (``settle`` asserts only in Debug), so the incremental
``opt_expr`` is checked against the full sweep here. For each file, smallest first, two children:

- A: ``O [--top T] -p "read_verilog --nolatches F; <phase 2 script>; write_rtlil a.il"``;
- B: the same, with ``--opt-full-rescan`` added to every ``opt`` and ``opt_expr`` command;

the script being ``tools/opt-check/phase2.script`` (``--script S``): the opt passes, ``proc``, the
main ``opt`` and ``compact``. The two ``write_rtlil`` files must be byte-identical. Every child
runs through ``memcap.run`` (``--mem-cap``, default 4G, one child at a time), in a work directory
(``--work DIR``, else a fresh temporary one that is left in place; the two files are overwritten
per design). A file that fails in both variants is an error, not a pass.

Exit codes: 0 every file identical; 1 a file differs; 2 a child failed or a usage error;
77 skipped (an ASan odin3 and no ``systemd-run``).
"""

from __future__ import annotations

import argparse
import filecmp
import sys
import tempfile
import time
from collections.abc import Sequence
from pathlib import Path

import elab_sweep
import memcap

TOOL = "rescan-compare"
HERE = Path(__file__).resolve().parent
DEFAULT_SCRIPT = HERE / "phase2.script"
RESCAN_FLAG = "--opt-full-rescan"
RESCAN_COMMANDS = ("opt", "opt_expr")
EXIT_OK, EXIT_DIFFER, EXIT_ERROR, EXIT_SKIP = 0, 1, 2, 77
TAIL_LINES = 5


def script_commands(text: str) -> list[str]:
    """A script file's commands: '#' comments and blank lines dropped, ';' and newline split."""
    commands: list[str] = []
    for line in text.splitlines():
        commands.extend(part.strip() for part in line.split("#", 1)[0].split(";") if part.strip())
    return commands


def with_rescan(commands: Sequence[str]) -> list[str]:
    """Every ``opt`` and ``opt_expr`` command with ``--opt-full-rescan`` added."""
    return [f"{command} {RESCAN_FLAG}" if command.split()[0] in RESCAN_COMMANDS else command
            for command in commands]


def pass_script(source: Path, commands: Sequence[str], out: str, techlib: str | None = None) -> str:
    read = [f"read_techlib {techlib}"] if techlib else []
    return "; ".join([*read, f"read_verilog --nolatches {source}", *commands, f"write_rtlil {out}"])


def exe(name: str) -> str:
    """The executable as an absolute path (the children run in the work directory); a bare name
    stays one, for PATH."""
    return str(Path(name).resolve()) if "/" in name else name


def tail(text: str) -> str:
    return "\n".join(text.strip().splitlines()[-TAIL_LINES:])


class Comparison:
    """The corpus through both variants."""

    def __init__(self, args: argparse.Namespace, commands: Sequence[str]) -> None:
        self.args = args
        self.cap: int = args.mem_cap
        self.commands = list(commands)
        self.work = Path(args.work) if args.work else Path(tempfile.mkdtemp(prefix="rescan-"))
        self.work.mkdir(parents=True, exist_ok=True)
        self.tops = elab_sweep.read_tops(Path(args.tops)) if args.tops else {}
        self.golden = Path(args.golden_root) if args.golden_root else None

    def variant(self, item: elab_sweep.Item, commands: Sequence[str], out: str) -> str | None:
        """Runs one variant; None on success, else why it failed."""
        top = elab_sweep.top_for(item, self.tops, self.golden)
        argv = [exe(self.args.odin3), *(["--top", top] if top else []), "-p",
                pass_script(item.path, commands, out, self.args.techlib)]
        try:
            done = memcap.run(argv, self.cap, capture_output=True, cwd=self.work)
        except (OSError, ValueError) as exc:
            return f"cannot run {argv[0]}: {exc}"
        if done.returncode == 0:
            return None
        return f"exit {done.returncode}: {tail((done.stdout or '') + (done.stderr or ''))}"

    def one(self, item: elab_sweep.Item) -> int:
        start = time.monotonic()
        first = self.variant(item, self.commands, "a.il")
        second = self.variant(item, with_rescan(self.commands), "b.il")
        secs = time.monotonic() - start
        if first is not None or second is not None:
            print(f"{TOOL}: {item.rel}: ERROR in {'opt' if first else 'opt --opt-full-rescan'}: "
                  f"{first or second}")
            return EXIT_ERROR
        if not filecmp.cmp(self.work / "a.il", self.work / "b.il", shallow=False):
            print(f"{TOOL}: {item.rel}: DIFFERS ({secs:.1f} s; {self.work}/a.il, b.il)")
            return EXIT_DIFFER
        print(f"{TOOL}: {item.rel}: identical ({secs:.1f} s)")
        return EXIT_OK

    def run(self, items: Sequence[elab_sweep.Item]) -> int:
        codes = [self.one(item) for item in items]
        differ, errors = codes.count(EXIT_DIFFER), codes.count(EXIT_ERROR)
        print(f"{TOOL}: {len(items)} files: {len(items) - differ - errors} identical, "
              f"{differ} differ, {errors} errors")
        return EXIT_ERROR if errors else EXIT_DIFFER if differ else EXIT_OK


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=__doc__.split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary (a Release build)")
    ap.add_argument("--mem-cap", type=elab_sweep.parse_size, default=4 << 30, metavar="SIZE",
                    help="per child (default 4G)")
    ap.add_argument("--work", metavar="DIR", help="the work directory (kept)")
    ap.add_argument("--tops", metavar="F", help="<file> <top> lines, as elab-sweep --tops")
    ap.add_argument("--golden-root", metavar="D", help="Parmys goldens, as elab-sweep")
    ap.add_argument("--techlib", metavar="F", type=lambda t: str(Path(t).resolve()),
                    help="read this tech library first (the hard blocks)")
    ap.add_argument("--root", metavar="R", help="paths are shown relative to R")
    ap.add_argument("--script", type=Path, default=DEFAULT_SCRIPT, help="the opt script")
    ap.add_argument("inputs", nargs="+", help="directories or files")
    return ap.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    try:
        commands = script_commands(Path(args.script).read_text(encoding="utf-8"))
        items = elab_sweep.collect(args.inputs, Path(args.root).resolve() if args.root else None)
        return Comparison(args, commands).run(items)
    except (OSError, elab_sweep.UsageError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_ERROR
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP


if __name__ == "__main__":
    sys.exit(main())

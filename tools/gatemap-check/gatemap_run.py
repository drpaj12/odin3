"""gatemap-run: one design through the gate flow with the corpus checks (gatemap spec 9; plan
Task 14).

Usage::

    gatemap-run --odin3 O --equiv-check E --lib NAME=FILE [--techlib FILE]... [--top T]
                [--work DIR] [--remap] [--mem-cap C] FILE...

Runs ``[read_techlib TECH;]... read_techlib FILE; read_verilog --nolatches FILE...; hierarchy; proc;
opt; lower --neg-edge keep; write_blif --gates-as-names rtl.blif; abc --gates NAME; dfflibmap NAME;
write_blif --gates-as-names gates.blif; write_blif cells.blif; write_verilog gates.v; dump_locs
--types locs.txt`` and requires, in this order:

1. odin3 exits 0 (a failure is the outcome ``FAIL odin3`` with exit 1, as in lower-run; a signal, a
   memory-cap kill or a timeout is exit 3, never an expected failure) and equiv-check ``rtl.blif
   gates.blif`` exits 0. Its exit 1 is the outcome ``differs``, 4 or 5 (ABC killed) ``unproven``
   (so is a checker killed by the memory cap or a signal), anything else (a netlist it cannot
   read, such as a flip-flop clocked by an internal net) ``error``, with equiv-check's last
   message as the detail. It runs with ``--blackbox adder,multiply,single_port_ram,dual_port_ram``
   (hard blocks are cut and paired) and, when it refuses an undriven net that reaches an output of
   the RTL, once more with ``--allow-undriven`` (the gate netlist keeps the net undriven too). The
   re-map leg runs with ``--timeout 1200``.
2. every ``node:TYPE`` of locs.txt is a cell of a library read (``--techlib`` files too: the hard
   blocks), a module of the dump, one of the
   ``$port_in``/``$port_out``/``$port_inout`` cells, or ``$_CONST0_ $_CONST1_ $_CONSTX_ $_CONSTZ_
   $_BUF_`` (else ``foreign-cell`` naming the first);
3. every library-cell node has a non-empty SRC (else ``noprov`` naming the first).

``--remap`` then reads the library and ``cells.blif`` (``write_blif`` of the gate netlist, library
cells as ``.subckt``; ``read_verilog -netlist`` cannot read ``gates.v`` back, see ``remap_script``)
and maps it again (``proc; lower; abc --gates NAME; dfflibmap NAME; write_blif --gates-as-names
remap.blif``) and
requires ``equiv-check gates.blif remap.blif`` to exit 0 as well (outcome ``remap-differs``,
``remap-unproven`` or ``remap-error`` otherwise; the remap is only run when the first leg is ok).

Prints ``gatemap-run: <outcome> <design>`` and ``gatemap-run: compared <0|1> of 1 designs``
(1 only when the outcome is ok). Exit codes: 0 ok; 1 odin3 failed, differs, foreign-cell, noprov,
error or a remap failure; 2 usage; 3 odin3 crashed or timed out; 4 unproven; 77 skipped (an ASan
child and no systemd-run).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

import lower_common
import memcap
import opt_equiv

TOOL = "gatemap-run"
EXIT_OK, EXIT_FAIL, EXIT_USAGE, EXIT_ODIN3, EXIT_UNPROVEN, EXIT_SKIP = 0, 1, 2, 3, 4, 77
EQUIV_OK, EQUIV_DIFFER, EQUIV_UNPROVEN, EQUIV_HARD = 0, 1, 4, 5
BLACKBOX_MODELS = "adder,multiply,single_port_ram,dual_port_ram"  # as tools/abc-check/abc_equiv.py
HARD_SUBCKT = re.compile(rf"^\.subckt +({BLACKBOX_MODELS.replace(',', '|')})(?= )", re.M)
UNDRIVEN = "undriven net"
REMAP_TIMEOUT = 1200  # equiv-check's limit in seconds for the re-map leg (the first leg keeps 600)
CAP_KILL = 137
DEFAULT_CAP = 2 << 30
TIMEOUT_SECONDS = 3600.0
PORT_CELLS = ("$port_in", "$port_out", "$port_inout")
CONSTANT_CELLS = ("$_CONST0_", "$_CONST1_", "$_CONSTX_", "$_CONSTZ_", "$_BUF_")
LIBRARY_CELL = re.compile(r"^\s*cell\s+(\S+)", re.M)
NODE_KIND = "node:"


@dataclass(frozen=True)
class Node:
    """One ``node:TYPE`` line of dump_locs: module, type, name and the SRC text ('' for none)."""

    module: str
    ctype: str
    name: str
    src: str


def library_cells(text: str) -> set[str]:
    """The cell names of an ``.o3lib`` text."""
    return set(LIBRARY_CELL.findall(text))


def parse_locs(text: str) -> tuple[set[str], list[Node]]:
    """(module names, node lines) of a ``dump_locs --types`` listing (``module|kind|name|SRC``)."""
    modules: set[str] = set()
    nodes: list[Node] = []
    for line in text.splitlines():
        parts = line.split("|", 3)
        if len(parts) < 3:
            continue
        if parts[1] == "module":
            modules.add(parts[2])
        elif parts[1].startswith(NODE_KIND):
            nodes.append(Node(parts[0], parts[1][len(NODE_KIND):], parts[2],
                              parts[3] if len(parts) == 4 else ""))
    return modules, nodes


def judge_cells(locs: str, cells: set[str]) -> tuple[str, str]:
    """("ok" | "foreign-cell" | "noprov", detail) of a dump_locs listing against the library."""
    modules, nodes = parse_locs(locs)
    allowed = cells | modules | {m.lstrip("\\") for m in modules} | set(PORT_CELLS) \
        | set(CONSTANT_CELLS)
    for node in nodes:
        if node.ctype not in allowed:
            return "foreign-cell", f"{node.ctype} ({node.module}/{node.name})"
    for node in nodes:
        if node.ctype in cells and not node.src:
            return "noprov", f"{node.ctype} ({node.module}/{node.name}) has no SRC"
    return "ok", ""


def flow_script(files: Sequence[str], top: str | None, techlibs: Sequence[str], lib: str,
                work: Path) -> str:
    """The ``-p`` script of the first leg: ``techlibs`` (the hard blocks first, the gate library
    last) are read before the design."""
    q = opt_equiv.quote
    front = [*(f"read_techlib {q(t)}" for t in techlibs[:-1]),
             *lower_common.read_front(files, top, techlibs[-1])]
    body = ["proc", "opt", "lower --neg-edge keep",
            f"write_blif --gates-as-names {q(str(work / 'rtl.blif'))}",
            f"abc --gates {lib}", f"dfflibmap {lib}",
            f"write_blif --gates-as-names {q(str(work / 'gates.blif'))}",
            f"write_blif {q(str(work / 'cells.blif'))}",
            f"write_verilog {q(str(work / 'gates.v'))}",
            f"dump_locs --types {q(str(work / 'locs.txt'))}"]
    return "; ".join([*front, *body])


def remap_script(techlibs: Sequence[str], lib: str, work: Path) -> str:
    """The ``-p`` script of the re-map leg: the gate netlist read back and mapped again. It is read
    as BLIF (``write_blif`` without ``--gates-as-names`` keeps the library cells as ``.subckt``):
    ``read_verilog -netlist`` cannot resolve a gate-kind library cell (only hard and black-box
    cell types resolve outside a module), so ``gates.v`` cannot be read back."""
    q = opt_equiv.quote
    return "; ".join([*(f"read_techlib {q(t)}" for t in techlibs),
                      f"read_blif {q(str(work / 'cells.blif'))}",
                      "proc", "lower", f"abc --gates {lib}", f"dfflibmap {lib}",
                      f"write_blif --gates-as-names {q(str(work / 'remap.blif'))}"])


def equiv_outcome(code: int) -> str:
    """The outcome word of an equiv-check exit code (5, ABC killed, gives no verdict: unproven)."""
    return {EQUIV_OK: "ok", EQUIV_DIFFER: "differs", EQUIV_UNPROVEN: "unproven",
            EQUIV_HARD: "unproven"}.get(code, "error")


def equiv_detail(text: str) -> str:
    """The last two lines of equiv-check's output that are not notices, joined by ' | ' and with
    the work directory dropped (the message of a refusal or a difference)."""
    lines = [ln.strip() for ln in text.splitlines() if ln.strip() and ": notice: " not in ln]
    return " | ".join(re.sub(r"\S*/(rtl|gates|remap)\.eq\.blif", r"\1.blif", ln)
                      for ln in lines[-2:])


EXIT_OF = {"ok": EXIT_OK, "unproven": EXIT_UNPROVEN, "remap-unproven": EXIT_UNPROVEN}


def exit_code(outcome: str) -> int:
    """The exit code of an outcome word (ok 0, unproven 4, every other failure 1)."""
    return EXIT_OF.get(outcome, EXIT_FAIL)


def parse(argv: Sequence[str] | None) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog=TOOL, description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("--odin3", required=True, help="the odin3 binary")
    ap.add_argument("--equiv-check", required=True, help="the equiv-check tool")
    ap.add_argument("--lib", required=True, metavar="NAME=FILE",
                    help="the library name and its .o3lib file")
    ap.add_argument("--techlib", action="append", default=[], metavar="FILE",
                    help="read this tech library (.o3lib) before the gate library: the hard blocks")
    ap.add_argument("--top", help="the top module")
    ap.add_argument("--remap", action="store_true", help="also map the gate netlist again")
    ap.add_argument("--mem-cap", type=opt_equiv.parse_size, default=DEFAULT_CAP, metavar="SIZE",
                    help="each child's cap (default 2G)")
    ap.add_argument("--work", type=Path, help="the working directory (default: a new one)")
    ap.add_argument("files", nargs="+", help="the Verilog files of one design")
    args = ap.parse_args(argv)
    name, sep, path = args.lib.partition("=")
    if not sep or not name or not path:
        ap.error(f"--lib {args.lib!r}: expected NAME=FILE")
    args.equiv_check = str(Path(args.equiv_check).resolve())
    for needed in (args.equiv_check, path, *args.techlib):
        if not Path(needed).is_file():
            ap.error(f"{needed}: no such file")
    return args


class OdinFailed(Exception):
    """odin3 exited non-zero (``hard`` false) or was killed by a signal, a memory cap or a
    timeout (``hard`` true)."""

    def __init__(self, message: str, hard: bool = False) -> None:
        super().__init__(message)
        self.hard = hard


def run_odin3(args: argparse.Namespace, script: str, work: Path) -> str:
    """Runs ``odin3 --check -p script``; its log, or OdinFailed."""
    top = ["--top", args.top] if args.top else []
    try:
        done = memcap.run([opt_equiv.exe(args.odin3), "--check", *top, "-p", script], args.mem_cap,
                          capture_output=True, cwd=work, timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as exc:
        raise OdinFailed(f"timed out after {TIMEOUT_SECONDS:.0f} s", hard=True) from exc
    log = (done.stdout or "") + (done.stderr or "")
    if done.returncode < 0 or done.returncode == CAP_KILL:
        raise OdinFailed(f"killed by signal {abs(done.returncode) % 128}: {opt_equiv.why(log)}",
                         hard=True)
    if done.returncode != 0:
        raise OdinFailed(f"odin3 exit {done.returncode}: {opt_equiv.why(log)}")
    return log


def run_equiv(args: argparse.Namespace, left: Path, right: Path, work: Path,
              extra: Sequence[str] = ()) -> tuple[str, str]:
    """(outcome word, one line of output) of ``equiv-check --blackbox M left right``."""
    try:
        done = memcap.run([args.equiv_check, "--blackbox", BLACKBOX_MODELS, *extra, str(left),
                           str(right)], args.mem_cap,
                          capture_output=True, cwd=work, timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return "unproven", f"timed out after {TIMEOUT_SECONDS:.0f} s"
    text = (done.stdout or "") + (done.stderr or "")
    detail = equiv_detail(text)
    if done.returncode < 0 or done.returncode == CAP_KILL:  # the memory cap killed the checker
        return "unproven", f"equiv-check killed by the memory cap or a signal ({done.returncode})"
    if done.returncode == EQUIV_HARD:
        detail = f"equiv-check exit {EQUIV_HARD} (ABC killed): {detail}"
    return equiv_outcome(done.returncode), detail


def hard_as_gate(text: str) -> str:
    """The BLIF text with each ``.subckt`` of a VTR hard block (no ``.model`` in the plain BLIF)
    spelled ``.gate``, the form equiv-check takes for a model-less cell."""
    return HARD_SUBCKT.sub(r".gate \1", text)


def gate_copy(path: Path) -> Path:
    """``path`` written as ``<name>.eq.blif`` with hard blocks as ``.gate``."""
    out = path.with_suffix(".eq.blif")
    out.write_text(hard_as_gate(path.read_text(encoding="utf-8")), encoding="utf-8")
    return out


def compare(args: argparse.Namespace, left: Path, right: Path, work: Path,
            limit: Sequence[str] = ()) -> tuple[str, str]:
    """run_equiv on the two BLIF files (hard blocks as ``.gate``), run again with
    ``--allow-undriven`` when the RTL itself has an undriven net that reaches an output (both
    netlists then read it as 0)."""
    left, right = gate_copy(left), gate_copy(right)
    outcome, detail = run_equiv(args, left, right, work, limit)
    if outcome == "error" and UNDRIVEN in detail:
        outcome, detail = run_equiv(args, left, right, work, (*limit, "--allow-undriven"))
    return outcome, detail


def check_design(args: argparse.Namespace, work: Path) -> tuple[str, str]:
    """(outcome word, detail) of one design."""
    name, _, path = args.lib.partition("=")
    lib_file = str(Path(path).resolve())
    techlibs = [*(str(Path(t).resolve()) for t in args.techlib), lib_file]
    files = [str(Path(f).resolve()) for f in args.files]
    run_odin3(args, flow_script(files, args.top, techlibs, name, work), work)
    outcome, detail = compare(args, work / "rtl.blif", work / "gates.blif", work)
    if outcome != "ok":
        return outcome, detail
    cells = set().union(*(library_cells(Path(t).read_text(encoding="utf-8")) for t in techlibs))
    outcome, detail = judge_cells((work / "locs.txt").read_text(encoding="utf-8"), cells)
    if outcome != "ok" or not args.remap:
        return outcome, detail
    run_odin3(args, remap_script(techlibs, name, work), work)
    outcome, detail = compare(args, work / "gates.blif", work / "remap.blif", work,
                              ("--timeout", str(REMAP_TIMEOUT)))
    return ("ok" if outcome == "ok" else f"remap-{outcome}"), detail


def main(argv: Sequence[str] | None = None) -> int:
    args = parse(argv)
    design = args.files[0]
    try:
        with tempfile.TemporaryDirectory(prefix="gatemap-run-") as tmp:
            work = Path(tmp) if args.work is None else args.work
            work.mkdir(parents=True, exist_ok=True)
            outcome, detail = check_design(args, work)
    except OdinFailed as exc:
        print(f"{TOOL}: {'CRASH' if exc.hard else 'FAIL'} odin3 {design}: {exc}")
        return EXIT_ODIN3 if exc.hard else EXIT_FAIL
    except memcap.CapUnavailable as exc:
        print(f"{TOOL}: skipped: {exc}", file=sys.stderr)
        return EXIT_SKIP
    except (OSError, opt_equiv.ToolError) as exc:
        print(f"{TOOL}: error: {exc}", file=sys.stderr)
        return EXIT_USAGE
    print(f"{TOOL}: {outcome} {design}" + (f": {detail}" if detail and outcome != "ok" else ""))
    print(f"{TOOL}: compared {int(outcome == 'ok')} of 1 designs")
    return exit_code(outcome)


if __name__ == "__main__":
    sys.exit(main())

"""pp-diff: compare Odin III's Verilog-2005 preprocessor with Icarus and Yosys (2B spec §10.4).

Usage::

    pp-diff [--odin3 BIN] [--iverilog BIN] [--yosys BIN] [-I DIR]... [-D NAME[=VALUE]]...
            [--suite REPO [--vtr DIR]] [--work DIR] [files...]

For each file, smallest first (``mcml.v`` last):

* Odin III: a generated ``.o3proj`` (``incdir``/``define`` lines, then ``file verilog <file>``)
  run as ``odin3 --project <it> -p "preprocess --out <dir>"``;
* Icarus: ``iverilog -E -g2005 -grelative-include [-I...] [-D...]``;
* Yosys: ``read_verilog -nosynthesis -ppdump [-I...] [-D...]``, the dump cut from its log (a
  ``\\`NAME`` left in the dump is Yosys's lexer error "undefined macro", so it counts as a
  rejection).

Odin III runs in one temporary directory, the oracles in another, empty one: Odin III never
searches the current directory for an include, Icarus and Yosys do (PHASE2 #11).
``-grelative-include`` makes Icarus search the including file's directory first, as Odin III
(PP-11) and Yosys do.  ``-I`` directories are made absolute before any tool sees them.

Each output is normalized the same way (PP-10 ``translate_off`` ... ``translate_on`` regions
removed; every pass-through directive line and every ``\\`line``/``\\`file_push``/
``\\`file_pop`` line stripped), then tokenized by one Verilog tokenizer (identifiers, numbers,
strings, operators; whitespace and comments dropped).  Odin III's raw stream must hold no
``translate_off`` comment (normalization would hide one).

Verdicts per file:

* Odin III rejects only by exit 1 with an ``error: <kind>: <detail>`` line; any other failure
  (signal, sanitizer abort, timeout, other exit code, no diagnostic, no stream) is CRASH.  An
  oracle killed by a signal or a timeout is CRASH too.  Icarus rejects by a nonzero exit or an
  error line on stderr (it exits 0 after some errors).
* Where the oracles agree (same tokens, or both reject), Odin III must match them: agree, else
  DIFFER.
* Where they disagree, ``ORACLES_DISAGREE`` names the oracle Odin III must match; a file
  missing from it or matching the other side is DISAGREE NOT EXPECTED.
* A declared divergence (spec §10.4) expects Odin III's error while both oracles accept; an
  Odin-III-only entry (``rec.v``: both oracles loop forever) runs Odin III alone.

``--suite REPO`` adds the standard input set: the reviewer probes, the pp-diff fixtures, every
micro under ``tests/micro/verilog`` holding a backtick, and the VTR set (``--vtr``, default
``$VTR_ROOT/vtr_flow/benchmarks/verilog``).  A micro that another micro ``\\`include``s is not
run on its own (it uses its includer's macros); Odin III's ``files-read.txt`` decides whether
it was compared through an includer or sits in a branch no includer selects.

Exit 0 when every file passes, 1 on any DIFFER, CRASH, DECLARED NOT MET or DISAGREE NOT
EXPECTED, 2 on a usage error or a missing odin3, 77 when an oracle or the VTR set is absent
(CTest skip).
"""

from __future__ import annotations

import argparse
import enum
import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterator, Sequence
from dataclasses import dataclass
from pathlib import Path

EXIT_OK = 0
EXIT_DIFFER = 1
EXIT_USAGE = 2
EXIT_SKIP = 77
TIMEOUT_S = 120

PASS_THROUGH = (
    "default_nettype",
    "timescale",
    "celldefine",
    "endcelldefine",
    "resetall",
    "begin_keywords",
    "end_keywords",
    "unconnected_drive",
    "nounconnected_drive",
    "pragma",
)
STRIPPED = (*PASS_THROUGH, "line", "file_push", "file_pop")

DUMP_BEGIN = "-- Verilog code after preprocessor --\n"
DUMP_END = "-- END OF DUMP --"

# Declared divergences (spec §10.4, PHASE2 #11): path suffix -> (the Odin III error expected,
# the reason).  Both oracles accept these files.
DECLARED: dict[str, tuple[str, str]] = {
    "tests/unit/data/pp/probes/t5.v": (
        "syntax: directive inside macro text",
        "PP-6: a conditional inside macro text (probe t5)",
    ),
    "tests/unit/data/pp/probes/u3.v": (
        "syntax: directive inside macro text",
        "PP-6: a `define inside macro text (probe u3)",
    ),
    "tests/unit/data/pp/probes/w1.v": (
        "syntax: recursive macro 'N'",
        "PP-7: recursion judged on the spelling ancestry (probe w1)",
    ),
    "tests/tools/fixtures/pp_diff/directive_name.v": (
        "syntax: compiler directive 'timescale' cannot be a macro name",
        "§7.3: a compiler directive name defined as a macro",
    ),
}

# Declared, Odin III only: path suffix -> (the Odin III error expected, why the oracles are not
# run).
ODIN3_ONLY: dict[str, tuple[str, str]] = {
    "tests/unit/data/pp/probes/rec.v": (
        "syntax: recursive macro 'R'",
        "both oracles loop forever on it (PP-7, measured 2026-10-10)",
    ),
}

ICARUS = "icarus"
YOSYS = "yosys"
# Files where the oracles disagree: path suffix -> (the oracle Odin III must match, the reason;
# spec §10.4 and the M7 list of PHASE2 #12).
ORACLES_DISAGREE: dict[str, tuple[str, str]] = {
    "tests/unit/data/pp/probes/t4.v": (
        YOSYS,
        "a // comment ending a `define with a trailing \\: Icarus continues the text",
    ),
    "tests/unit/data/pp/probes/t6.v": (YOSYS, "Icarus substitutes formals inside strings"),
    "tests/unit/data/pp/probes/t7.v": (YOSYS, "a /* */ in a body: Icarus pastes its neighbours"),
    "tests/unit/data/pp/probes/t8.v": (YOSYS, "an undefined macro: Icarus substitutes nothing"),
    "tests/unit/data/pp/probes/t9.v": (ICARUS, "a newline before the (: Yosys rejects"),
    "tests/unit/data/pp/probes/u1.v": (
        ICARUS,
        "a multi-line /* */ in a `define: Yosys keeps it, Icarus reports it",
    ),
    "tests/unit/data/pp/probes/w2.v": (YOSYS, "`define F() with an empty formal list"),
    "tests/unit/data/pp/probes/w5.v": (ICARUS, "a newline before the (: Yosys rejects"),
    "tests/micro/verilog/syntax/preprocessor_define.v": (
        YOSYS,
        "`ADDITION_num (PHASE2 #11): Icarus substitutes nothing",
    ),
    "tests/micro/verilog/syntax/preprocessor_complex_define.v": (
        YOSYS,
        "`ADDITION_num (PHASE2 #11): Icarus substitutes nothing",
    ),
    "tests/micro/verilog/syntax/preprocessor_define_with_comment.v": (
        YOSYS,
        "`ADDITION_num (PHASE2 #11): Icarus substitutes nothing",
    ),
}

PROBES = "tests/unit/data/pp/probes"
FIXTURES = "tests/tools/fixtures/pp_diff"
MICROS = "tests/micro/verilog"
VTR_SUBDIR = "vtr_flow/benchmarks/verilog"
DEFAULT_VTR_ROOT = Path.home() / "odin3-ws/external/vtr-verilog-to-routing"
DEFAULT_YOSYS = Path.home() / "odin3-ws/external/yosys/build/yosys"
LAST = "mcml.v"

# ---- lexing ------------------------------------------------------------------------------

_OPERATORS = (
    "<<<", ">>>", "===", "!==",
    "**", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "~&", "~|", "~^", "^~",
    "->", "+:", "-:", "(*", "*)",
)  # fmt: skip
_LEX = re.compile(
    r"(?P<ws>[ \t\r\n\f\v]+)"
    r"|(?P<lc>//[^\n]*)"
    r"|(?P<bc>/\*.*?(?:\*/|\Z))"
    r'|(?P<str>"(?:[^"\\\n]|\\.)*"?)'
    r"|(?P<esc>\\[^ \t\r\n\f\v]+)"
    r"|(?P<dir>`[A-Za-z_][A-Za-z0-9_$]*)"
    r"|(?P<base>'[sS]?[bBoOdDhH][ \t]*[0-9a-zA-Z_?]+)"
    r"|(?P<num>[0-9][0-9_]*(?:\.[0-9][0-9_]*)?(?:[eE][+-]?[0-9][0-9_]*)?)"
    r"|(?P<id>[A-Za-z_$][A-Za-z0-9_$]*)"
    r"|(?P<op>" + "|".join(re.escape(op) for op in _OPERATORS) + r")"
    r"|(?P<other>.)",
    re.DOTALL,
)
_XOFF_LINE = re.compile(r"//[ \t]*(?:synopsys|synthesis|pragma)[ \t]*translate_(off|on)[ \t\r]*")
_XOFF_BLOCK = re.compile(r"/\*[ \t]*(?:synopsys|synthesis|pragma)[ \t]*translate_(off|on)[ \t]*\*/")
_BLANKS = re.compile(r"[ \t]+")


def lex(text: str) -> Iterator[tuple[str, str]]:
    """Every lexeme of ``text`` as (kind, text); the lexemes concatenate back to ``text``."""
    for match in _LEX.finditer(text):
        kind = match.lastgroup
        assert kind is not None
        yield kind, match.group()


def _xoff(kind: str, lexeme: str) -> str | None:
    """'off' or 'on' when the comment is a PP-10 translate pragma, else None."""
    pattern = _XOFF_LINE if kind == "lc" else _XOFF_BLOCK if kind == "bc" else None
    match = pattern.fullmatch(lexeme) if pattern is not None else None
    return match.group(1) if match is not None else None


def normalize(text: str) -> str:
    """``text`` without its PP-10 regions (the pragma comments included) and with the rest of
    every line starting at a pass-through, `line, `file_push or `file_pop directive dropped (the
    newline that ends it is kept)."""
    out: list[str] = []
    in_region = False
    in_directive = False
    for kind, lexeme in lex(text):
        pragma = _xoff(kind, lexeme)
        if in_region:
            in_region = pragma != "on"
            continue
        if pragma == "off":
            in_region = True
            continue
        if in_directive:
            if "\n" not in lexeme:
                continue
            in_directive = False
        if kind == "dir" and lexeme[1:] in STRIPPED:
            in_directive = True
            continue
        out.append(lexeme)
    return "".join(out)


def tokenize(text: str) -> list[str]:
    """The tokens of ``text``: whitespace and comments dropped, blanks inside a based literal's
    base and value removed."""
    tokens: list[str] = []
    for kind, lexeme in lex(text):
        if kind in ("ws", "lc", "bc"):
            continue
        tokens.append(_BLANKS.sub("", lexeme) if kind == "base" else lexeme)
    return tokens


# ---- outcomes and classification -------------------------------------------------------------


@dataclass(frozen=True)
class Outcome:
    """One tool's result on one file: its normalized tokens, its error message, or a crash."""

    ok: bool
    tokens: tuple[str, ...] = ()
    message: str = ""
    crashed: bool = False

    @staticmethod
    def accepted(tokens: Sequence[str]) -> Outcome:
        return Outcome(True, tuple(tokens))

    @staticmethod
    def rejected(message: str) -> Outcome:
        return Outcome(False, (), message)

    @staticmethod
    def crash(message: str) -> Outcome:
        return Outcome(False, (), message, crashed=True)

    def same(self, other: Outcome) -> bool:
        """Equal tokens, or both rejected (a crash is never the same as anything)."""
        if self.crashed or other.crashed:
            return False
        return self.ok == other.ok and (not self.ok or self.tokens == other.tokens)


class Kind(enum.Enum):
    AGREE = "agree"
    ORACLES_DISAGREE = "oracles disagree"
    DECLARED = "declared"
    EXCLUDED = "excluded"
    DIFFER = "DIFFER"
    CRASH = "CRASH"
    DECLARED_UNMET = "DECLARED NOT MET"
    DISAGREE_UNEXPECTED = "DISAGREE NOT EXPECTED"


FAILURES = (Kind.DIFFER, Kind.CRASH, Kind.DECLARED_UNMET, Kind.DISAGREE_UNEXPECTED)


@dataclass(frozen=True)
class Expect:
    """What a file is expected to show: nothing special, a declared Odin III error (``declared``),
    or the oracle Odin III must match where the oracles disagree (``side``)."""

    declared: str | None = None
    side: str | None = None


def first_difference(a: Sequence[str], b: Sequence[str]) -> int | None:
    """The first token index where ``a`` and ``b`` differ, None when equal."""
    for i, (x, y) in enumerate(zip(a, b, strict=False)):
        if x != y:
            return i
    return None if len(a) == len(b) else min(len(a), len(b))


def _context(tokens: Sequence[str], index: int) -> str:
    return " ".join(tokens[max(0, index - 3) : index + 4]) or "<end>"


def describe(name: str, outcome: Outcome, reference: Outcome) -> str:
    """How ``outcome`` differs from ``reference`` (named ``name``)."""
    if not outcome.ok:
        return f"rejects: {outcome.message}"
    if not reference.ok:
        return f"accepts; {name} rejects: {reference.message}"
    index = first_difference(outcome.tokens, reference.tokens)
    return (
        f"token {index}: '{_context(outcome.tokens, index or 0)}' vs {name} "
        f"'{_context(reference.tokens, index or 0)}'"
    )


def _side(odin3: Outcome, icarus: Outcome, yosys: Outcome) -> str:
    return ICARUS if odin3.same(icarus) else YOSYS if odin3.same(yosys) else "neither"


def _disagreement(
    odin3: Outcome, icarus: Outcome, yosys: Outcome, side: str | None
) -> tuple[Kind, str]:
    if icarus.ok != yosys.ok:
        accepts, rejects = (ICARUS, yosys) if icarus.ok else (YOSYS, icarus)
        detail = f"{accepts} accepts, the other rejects: {rejects.message}"
    else:
        detail = "icarus " + describe("yosys", icarus, yosys)
    got = _side(odin3, icarus, yosys)
    if side is None:
        return Kind.DISAGREE_UNEXPECTED, f"{detail}; odin3 = {got}; no ORACLES_DISAGREE row"
    if got != side:
        return Kind.DISAGREE_UNEXPECTED, f"{detail}; odin3 = {got}, expected {side}"
    return Kind.ORACLES_DISAGREE, f"{detail}; odin3 = {got}"


def _crash(odin3: Outcome, icarus: Outcome, yosys: Outcome) -> str | None:
    for name, outcome in (("odin3", odin3), (ICARUS, icarus), (YOSYS, yosys)):
        if outcome.crashed:
            return f"{name}: {outcome.message}"
    return None


def _declared(odin3: Outcome, icarus: Outcome, yosys: Outcome, declared: str) -> tuple[Kind, str]:
    if not (icarus.ok and yosys.ok and icarus.same(yosys)):
        oracles = "icarus " + describe("yosys", icarus, yosys) if icarus.ok else icarus.message
        return Kind.DECLARED_UNMET, f"the oracles no longer both accept alike: {oracles}"
    if not odin3.ok and declared in odin3.message:
        return Kind.DECLARED, f"{declared} (both oracles accept)"
    got = odin3.message if not odin3.ok else "odin3 accepts"
    return Kind.DECLARED_UNMET, f"expected '{declared}', got: {got}"


def classify(
    odin3: Outcome, icarus: Outcome, yosys: Outcome, expect: Expect | None = None
) -> tuple[Kind, str]:
    """The verdict on one file and a one-line detail."""
    expect = expect if expect is not None else Expect()
    crash = _crash(odin3, icarus, yosys)
    if crash is not None:
        return Kind.CRASH, crash
    if expect.declared is not None:
        return _declared(odin3, icarus, yosys, expect.declared)
    if not icarus.same(yosys):
        return _disagreement(odin3, icarus, yosys, expect.side)
    if odin3.same(icarus):
        return Kind.AGREE, "all reject" if not odin3.ok else f"{len(odin3.tokens)} tokens"
    return Kind.DIFFER, "odin3 " + describe("oracles", odin3, icarus)


def classify_odin3_only(odin3: Outcome, declared: str) -> tuple[Kind, str]:
    """The verdict on an Odin-III-only declared file."""
    if odin3.crashed:
        return Kind.CRASH, f"odin3: {odin3.message}"
    if not odin3.ok and declared in odin3.message:
        return Kind.DECLARED, f"{declared} (oracles not run)"
    got = odin3.message if not odin3.ok else "odin3 accepts"
    return Kind.DECLARED_UNMET, f"expected '{declared}', got: {got}"


# ---- the three tools ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Options:
    odin3: str
    iverilog: str
    yosys: str
    incdirs: tuple[str, ...]
    defines: tuple[str, ...]


@dataclass(frozen=True)
class Run:
    code: int | None  # None: timed out
    stdout: str
    stderr: str


def _run(argv: Sequence[str], cwd: Path) -> Run:
    try:
        proc = subprocess.run(
            argv, cwd=cwd, capture_output=True, text=True, errors="replace", timeout=TIMEOUT_S
        )
    except subprocess.TimeoutExpired:
        return Run(None, "", "")
    return Run(proc.returncode, proc.stdout, proc.stderr)


def _abnormal(run: Run) -> str | None:
    """Why ``run`` ended abnormally (timeout or signal), None for a normal exit."""
    if run.code is None:
        return f"timeout after {TIMEOUT_S} s"
    if run.code < 0:
        return f"killed by signal {-run.code}"
    return None


def _first_line(text: str, pattern: re.Pattern[str]) -> str | None:
    return next((line.strip() for line in text.splitlines() if pattern.search(line)), None)


_ODIN3_ERROR = re.compile(r"error: [a-z_]+: \S")
_ODIN3_EXIT_ERROR = 1
# Icarus's error texts (it exits 0 after some of them); warnings are not errors.
_ICARUS_ERROR = re.compile(r"(?i)error|unterminated|malformed")
_WARNING = re.compile(r"(?i)warning")


def odin3_outcome(run: Run, stream: Path) -> Outcome:
    """Odin III's verdict: exit 0 with a stream accepts, exit 1 with an ``error: kind: detail``
    line rejects, anything else is a crash."""
    abnormal = _abnormal(run)
    if abnormal is not None:
        return Outcome.crash(abnormal)
    if run.code == 0 and stream.is_file():
        text = stream.read_text(errors="replace")
        left = [lx for kind, lx in lex(text) if _xoff(kind, lx) == "off"]
        if left:
            return Outcome.crash(f"a translate_off comment left in the stream: {left[0]!r}")
        return Outcome.accepted(tokenize(normalize(text)))
    error = _first_line(run.stderr, _ODIN3_ERROR)
    if run.code == _ODIN3_EXIT_ERROR and error is not None:
        return Outcome.rejected(error)
    what = "no stream" if run.code == 0 else f"exit {run.code}"
    tail = run.stderr.strip().splitlines()
    return Outcome.crash(f"{what}, no located error: {tail[-1] if tail else '(no stderr)'}")


class UsageError(Exception):
    """A command line pp-diff cannot run (exit EXIT_USAGE)."""


def _word(path: str) -> str:
    """``path`` as one word of a Yosys script or an ``.o3proj`` line: double-quoted when it holds
    a blank, ``;`` or ``#``. Neither grammar has an escape, so a ``"`` is a usage error."""
    if '"' in path:
        raise UsageError(f"{path}: a path holding '\"' cannot be passed to Yosys or odin3")
    return f'"{path}"' if any(c.isspace() or c in ';#' for c in path) else path


def _check_words(opts: Options, files: Sequence[Path]) -> str | None:
    """The usage error for an include directory or input file _word cannot carry, if any."""
    try:
        for path in [*opts.incdirs, *map(str, files)]:
            _word(path)
    except UsageError as exc:
        return str(exc)
    return None


def run_odin3(opts: Options, source: Path, work: Path) -> tuple[Outcome, list[Path] | None]:
    """Odin III on ``source`` from ``work``; also the files it read (None when it failed)."""
    project = work / "pp.o3proj"
    lines = [f"incdir {_word(d)}" for d in opts.incdirs]
    lines += [f"define {d}" for d in opts.defines]
    lines.append(f"file verilog {_word(str(source))}")
    project.write_text("\n".join(lines) + "\n")
    out = work / "odin3-out"
    run = _run([opts.odin3, "--project", str(project), "-p", f"preprocess --out {out}"], work)
    outcome = odin3_outcome(run, out / "0.v")
    read = out / "files-read.txt"
    files = [Path(line) for line in read.read_text().splitlines()] if read.is_file() else None
    return outcome, files if outcome.ok else None


def icarus_outcome(run: Run, out: Path) -> Outcome:
    """Icarus's verdict: a nonzero exit or an error line on stderr rejects (it exits 0 after
    some errors, e.g. "Unterminated comment in define"); a signal or timeout is a crash."""
    abnormal = _abnormal(run)
    if abnormal is not None:
        return Outcome.crash(abnormal)
    errors = [ln for ln in run.stderr.splitlines() if not _WARNING.search(ln)]
    error = next((ln.strip() for ln in errors if _ICARUS_ERROR.search(ln)), None)
    if run.code != 0 or error is not None or not out.is_file():
        tail = run.stderr.strip().splitlines()
        return Outcome.rejected(error or (tail[-1] if tail else f"exit {run.code}"))
    return Outcome.accepted(tokenize(normalize(out.read_text(errors="replace"))))


def run_icarus(opts: Options, source: Path, cwd: Path, out_dir: Path) -> Outcome:
    out = out_dir / "icarus.v"
    argv = [opts.iverilog, "-E", "-g2005", "-grelative-include", "-o", str(out)]
    argv += [f"-I{d}" for d in opts.incdirs] + [f"-D{d}" for d in opts.defines]
    return icarus_outcome(_run([*argv, str(source)], cwd), out)


def yosys_dump(log: str) -> str | None:
    """The preprocessed text from a Yosys log with ``-ppdump``, None when there is none."""
    begin = log.find(DUMP_BEGIN)
    if begin < 0:
        return None
    end = log.rfind(DUMP_END, begin)
    return log[begin + len(DUMP_BEGIN) : end if end >= 0 else len(log)]


def yosys_outcome(log: str, stderr: str) -> Outcome:
    """Yosys's verdict: no dump is a preprocessor error; a backtick word left in the dump is the
    lexer's "undefined macro" error; otherwise the dump's tokens (a later parse error does not
    concern the preprocessor)."""
    dump = yosys_dump(log)
    if dump is None:
        line = _first_line(stderr + "\n" + log, re.compile("ERROR"))
        return Outcome.rejected(line or "no dump")
    tokens = tokenize(normalize(dump))
    left = [t for t in tokens if t.startswith("`")]
    if left:
        return Outcome.rejected(f"undefined macro {left[0]} (left in the dump)")
    return Outcome.accepted(tokens)


def run_yosys(opts: Options, source: Path, cwd: Path) -> Outcome:
    args = ["-nosynthesis", "-ppdump"]
    args += [f"-I{d}" for d in opts.incdirs] + [f"-D{d}" for d in opts.defines]
    script = "read_verilog " + " ".join(_word(a) for a in [*args, str(source)])
    run = _run([opts.yosys, "-Q", "-T", "-p", script], cwd)
    abnormal = _abnormal(run)
    if abnormal is not None:
        return Outcome.crash(abnormal)
    return yosys_outcome(run.stdout, run.stderr)


# ---- input set ---------------------------------------------------------------------------------

_INCLUDE = re.compile(r'`include\s*"([^"\n]+)"')


def _includes(path: Path, text: str) -> list[Path]:
    """The files ``text`` names in an `include outside comments (resolved as written)."""
    code = "".join(lx for kind, lx in lex(text) if kind not in ("lc", "bc"))
    return [
        Path(os.path.normpath(path.parent / match.group(1))) for match in _INCLUDE.finditer(code)
    ]


def backtick_roots(root: Path) -> tuple[list[Path], dict[Path, list[Path]]]:
    """The .v/.vh files under ``root`` holding a backtick, split into roots (sorted) and the
    files other ones include (each mapped to its includers, sorted)."""
    files = sorted(p for p in root.rglob("*") if p.suffix in (".v", ".vh") and p.is_file())
    texts = {p: p.read_text(errors="replace") for p in files}
    included: dict[Path, list[Path]] = {}
    for path in files:
        for name in _includes(path, texts[path]):
            if name in texts and "`" in texts[name]:
                included.setdefault(name, []).append(path)
    roots = [p for p in files if "`" in texts[p] and p not in included]
    return roots, included


def coverage(
    included: dict[Path, list[Path]], reads: dict[Path, list[Path] | None]
) -> list[tuple[Path, str]]:
    """Why each included micro is not run on its own: compared through the first includer whose
    Odin III run read it, or read by none (an untaken conditional branch, or a rejected
    includer).  ``reads`` maps each run file to the files it read (None: Odin III failed)."""
    rows = []
    for path, includers in sorted(included.items()):
        by = next((root for root, files in reads.items() if files and path in files), None)
        if by is not None:
            rows.append((path, f"included by {by}; compared through it"))
            continue
        names = ", ".join(str(p) for p in includers)
        failed = [p for p in includers if p in reads and reads[p] is None]
        if failed:
            reason = f"its includer {failed[0]} is rejected by odin3; never compared"
        else:
            reason = f"a variant that its includer's conditional does not select ({names})"
        rows.append((path, reason))
    return rows


def smallest_first(paths: Sequence[Path]) -> list[Path]:
    """Ascending size (then name), any mcml.v last; duplicates dropped."""
    unique = list(dict.fromkeys(paths))
    return sorted(unique, key=lambda p: (p.name == LAST, p.stat().st_size, str(p)))


def _suffix_key(path: Path, table: dict[str, object]) -> str | None:
    text = path.as_posix()
    return next((key for key in table if text.endswith("/" + key) or text == key), None)


def suite(repo: Path, vtr: Path) -> tuple[list[Path], dict[Path, list[Path]]]:
    """The standard input set and the included micros not run on their own."""
    files = sorted((repo / PROBES).glob("*.v")) + sorted((repo / FIXTURES).glob("*.v"))
    roots, included = backtick_roots(repo / MICROS)
    files += roots + sorted(vtr.glob("*.v"))
    return smallest_first(files), included


# ---- driver ------------------------------------------------------------------------------------


def expectation(source: Path) -> Expect:
    key = _suffix_key(source, dict(DECLARED))
    if key is not None:
        return Expect(declared=DECLARED[key][0])
    key = _suffix_key(source, dict(ORACLES_DISAGREE))
    return Expect(side=ORACLES_DISAGREE[key][0] if key is not None else None)


def check_file(
    opts: Options, source: Path, scratch: Path
) -> tuple[Kind, str, list[Path] | None]:
    """Run the tools on ``source`` (Odin III in one directory, the oracles from another, empty
    one) and classify the result; also the files Odin III read."""
    with tempfile.TemporaryDirectory(dir=scratch) as tmp:
        work = Path(tmp)
        for sub in ("odin3", "oracle-cwd", "oracle-out"):
            (work / sub).mkdir()
        odin3, files = run_odin3(opts, source, work / "odin3")
        only = _suffix_key(source, dict(ODIN3_ONLY))
        if only is not None:
            kind, detail = classify_odin3_only(odin3, ODIN3_ONLY[only][0])
            return kind, detail, files
        icarus = run_icarus(opts, source, work / "oracle-cwd", work / "oracle-out")
        yosys = run_yosys(opts, source, work / "oracle-cwd")
    kind, detail = classify(odin3, icarus, yosys, expectation(source))
    return kind, detail, files


def _parse(argv: Sequence[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(prog="pp-diff", description=__doc__.split("\n")[0])
    ap.add_argument("--odin3", default="odin3")
    ap.add_argument("--iverilog", default="/usr/bin/iverilog")
    ap.add_argument("--yosys", default=str(DEFAULT_YOSYS))
    ap.add_argument("-I", dest="incdirs", action="append", default=[], metavar="DIR")
    ap.add_argument("-D", dest="defines", action="append", default=[], metavar="NAME[=VALUE]")
    ap.add_argument("--suite", type=Path, metavar="REPO", help="add the standard input set")
    ap.add_argument("--vtr", type=Path, metavar="DIR", help="the VTR benchmark directory")
    ap.add_argument("--work", type=Path, metavar="DIR", help="parent of the scratch directory")
    ap.add_argument("files", nargs="*", type=Path)
    return ap.parse_args(argv)


def options(args: argparse.Namespace) -> Options:
    """The tool options, every -I directory made absolute (the tools run elsewhere)."""
    incdirs = tuple(os.path.abspath(d) for d in args.incdirs)
    return Options(args.odin3, args.iverilog, args.yosys, incdirs, tuple(args.defines))


def _missing_oracle(opts: Options) -> str | None:
    return next((t for t in (opts.iverilog, opts.yosys) if shutil.which(t) is None), None)


def _inputs(args: argparse.Namespace) -> tuple[list[Path], dict[Path, list[Path]]] | str:
    files = [p.resolve() for p in args.files]
    included: dict[Path, list[Path]] = {}
    if args.suite is not None:
        vtr_root = os.environ.get("VTR_ROOT", str(DEFAULT_VTR_ROOT))
        vtr = args.vtr if args.vtr is not None else Path(vtr_root) / VTR_SUBDIR
        if not vtr.is_dir():
            return f"VTR benchmark directory {vtr}"
        more, included = suite(args.suite.resolve(), vtr.resolve())
        files += more
    return smallest_first(files), included


def _report(results: list[tuple[Path, Kind, str]], excluded: list[tuple[Path, str]]) -> int:
    counts = {kind: 0 for kind in Kind}
    for kind in (Kind.ORACLES_DISAGREE, Kind.DECLARED, *FAILURES):
        rows = [(p, d) for p, k, d in results if k is kind]
        if rows:
            print(f"\n{kind.value} ({len(rows)}):")
            for path, detail in rows:
                print(f"  {path}: {detail}")
    if excluded:
        print(f"\nexcluded ({len(excluded)}):")
        for path, reason in excluded:
            print(f"  {path}: {reason}")
    for _, kind, _ in results:
        counts[kind] += 1
    counts[Kind.EXCLUDED] = len(excluded)
    print("\nsummary: " + ", ".join(f"{k.value} {n}" for k, n in counts.items()))
    return EXIT_DIFFER if any(counts[k] for k in FAILURES) else EXIT_OK


def _usable(opts: Options, inputs: object) -> int | None:
    """An exit code when the run cannot start: odin3 missing is an error (it is under test), a
    missing oracle or VTR set a skip."""
    if shutil.which(opts.odin3) is None:
        print(f"pp-diff: odin3 binary {opts.odin3} not found", file=sys.stderr)
        return EXIT_USAGE
    missing = _missing_oracle(opts) or (inputs if isinstance(inputs, str) else None)
    if missing is not None:
        print(f"pp-diff: skipped: {missing} not found", file=sys.stderr)
        return EXIT_SKIP
    return None


def main(argv: Sequence[str]) -> int:
    args = _parse(argv)
    opts = options(args)
    inputs = _inputs(args)
    code = _usable(opts, inputs)
    if code is not None or isinstance(inputs, str):
        return code if code is not None else EXIT_USAGE
    files, included = inputs
    if not files:
        print("pp-diff: no input files", file=sys.stderr)
        return EXIT_USAGE
    unquotable = _check_words(opts, files)
    if unquotable is not None:
        print(f"pp-diff: {unquotable}", file=sys.stderr)
        return EXIT_USAGE
    results: list[tuple[Path, Kind, str]] = []
    reads: dict[Path, list[Path] | None] = {}
    with tempfile.TemporaryDirectory(prefix="pp-diff-", dir=args.work) as scratch:
        for path in files:
            kind, detail, read = check_file(opts, path, Path(scratch))
            print(f"{kind.value:16} {path} ({detail})", flush=True)
            results.append((path, kind, detail))
            reads[path] = read
    return _report(results, coverage(included, reads))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

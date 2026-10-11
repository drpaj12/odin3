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

All three run from a neutral, empty working directory: Odin III never searches the current
directory for an include, Icarus and Yosys do (PHASE2 #11).  ``-grelative-include`` makes Icarus
search the including file's directory first, as Odin III (PP-11) and Yosys do.

Each output is normalized the same way (PP-10 ``translate_off`` ... ``translate_on`` regions
removed; every pass-through directive line and every ``\\`line``/``\\`file_push``/
``\\`file_pop`` line stripped), then tokenized by one Verilog tokenizer (identifiers, numbers,
strings, operators; whitespace and comments dropped).  Odin III is compared only where the two
oracles agree (same tokens, or both reject); otherwise the file is listed under "oracles
disagree".  A declared divergence (spec §10.4) expects Odin III's error and fails otherwise.

``--suite REPO`` adds the standard input set: the reviewer probes (``rec.v`` excluded), the
pp-diff fixtures, every micro under ``tests/micro/verilog`` holding a backtick (a file included by
another micro is compared through its includer), and the VTR set (``--vtr``, default
``$VTR_ROOT/vtr_flow/benchmarks/verilog``).

Exit 0 when nothing differs, 1 on an undeclared disagreement with an agreeing oracle pair or an
unmet declared divergence, 2 on a usage error, 77 when a tool or the VTR set is absent (CTest
skip).
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
TIMEOUT_S = 300

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

# Files of the --suite set that are not run: path suffix -> reason.
EXCLUDED: dict[str, str] = {
    "tests/unit/data/pp/probes/rec.v": (
        "both oracles loop forever on it (PP-7, measured 2026-10-10); Odin III's rejection is "
        "unit-tested (test_pp_expand)"
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
    """One tool's result on one file: its normalized tokens, or its error message."""

    ok: bool
    tokens: tuple[str, ...] = ()
    message: str = ""

    @staticmethod
    def accepted(tokens: Sequence[str]) -> Outcome:
        return Outcome(True, tuple(tokens))

    @staticmethod
    def rejected(message: str) -> Outcome:
        return Outcome(False, (), message)

    def same(self, other: Outcome) -> bool:
        """Equal tokens, or both rejected."""
        return self.ok == other.ok and (not self.ok or self.tokens == other.tokens)


class Kind(enum.Enum):
    AGREE = "agree"
    ORACLES_DISAGREE = "oracles disagree"
    DECLARED = "declared"
    DECLARED_UNMET = "DECLARED NOT MET"
    DIFFER = "DIFFER"
    EXCLUDED = "excluded"


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


def _disagreement(odin3: Outcome, icarus: Outcome, yosys: Outcome) -> str:
    if icarus.ok != yosys.ok:
        accepts, rejects = ("icarus", yosys) if icarus.ok else ("yosys", icarus)
        detail = f"{accepts} accepts, the other rejects: {rejects.message}"
    else:
        detail = "icarus " + describe("yosys", icarus, yosys)
    side = "icarus" if odin3.same(icarus) else "yosys" if odin3.same(yosys) else "neither"
    return f"{detail}; odin3 = {side}"


def classify(
    odin3: Outcome, icarus: Outcome, yosys: Outcome, declared: str | None
) -> tuple[Kind, str]:
    """The verdict on one file and a one-line detail.  ``declared`` is the Odin III error a
    declared divergence expects (a substring of its message), None for an ordinary file."""
    if declared is not None:
        if not odin3.ok and declared in odin3.message:
            pair = "oracles agree" if icarus.same(yosys) else "oracles disagree"
            return Kind.DECLARED, f"{declared} ({pair})"
        got = odin3.message if not odin3.ok else "odin3 accepts"
        return Kind.DECLARED_UNMET, f"expected '{declared}', got: {got}"
    if not icarus.same(yosys):
        return Kind.ORACLES_DISAGREE, _disagreement(odin3, icarus, yosys)
    if odin3.same(icarus):
        return Kind.AGREE, "all reject" if not odin3.ok else f"{len(odin3.tokens)} tokens"
    return Kind.DIFFER, "odin3 " + describe("oracles", odin3, icarus)


# ---- the three tools ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Options:
    odin3: str
    iverilog: str
    yosys: str
    incdirs: tuple[str, ...]
    defines: tuple[str, ...]


def _run(argv: Sequence[str], cwd: Path) -> tuple[int, str, str]:
    try:
        proc = subprocess.run(
            argv, cwd=cwd, capture_output=True, text=True, errors="replace", timeout=TIMEOUT_S
        )
    except subprocess.TimeoutExpired:
        return -1, "", f"timeout after {TIMEOUT_S} s"
    return proc.returncode, proc.stdout, proc.stderr


def _error_line(stderr: str, marker: str) -> str:
    lines = [line for line in stderr.splitlines() if marker in line]
    if lines:
        return lines[0].strip()
    tail = stderr.strip().splitlines()
    return tail[-1] if tail else "failed without a message"


def _word(path: str) -> str:
    return f'"{path}"' if any(c.isspace() for c in path) else path


def run_odin3(opts: Options, source: Path, work: Path) -> Outcome:
    project = work / "pp.o3proj"
    lines = [f"incdir {_word(os.path.abspath(d))}" for d in opts.incdirs]
    lines += [f"define {d}" for d in opts.defines]
    lines.append(f"file verilog {_word(str(source))}")
    project.write_text("\n".join(lines) + "\n")
    out = work / "odin3-out"
    code, _, stderr = _run(
        [opts.odin3, "--project", str(project), "-p", f"preprocess --out {out}"], work
    )
    stream = out / "0.v"
    if code != 0 or not stream.is_file():
        return Outcome.rejected(_error_line(stderr, "error:"))
    return Outcome.accepted(tokenize(normalize(stream.read_text(errors="replace"))))


def run_icarus(opts: Options, source: Path, work: Path) -> Outcome:
    out = work / "icarus.v"
    argv = [opts.iverilog, "-E", "-g2005", "-grelative-include", "-o", str(out)]
    argv += [f"-I{d}" for d in opts.incdirs] + [f"-D{d}" for d in opts.defines]
    code, _, stderr = _run([*argv, str(source)], work)
    if code != 0 or not out.is_file():
        return Outcome.rejected(_error_line(stderr, "error"))
    return Outcome.accepted(tokenize(normalize(out.read_text(errors="replace"))))


def yosys_dump(log: str) -> str | None:
    """The preprocessed text from a Yosys log with ``-ppdump``, None when there is none."""
    begin = log.find(DUMP_BEGIN)
    if begin < 0:
        return None
    end = log.find(DUMP_END, begin)
    return log[begin + len(DUMP_BEGIN) : end if end >= 0 else len(log)]


def yosys_outcome(log: str, stderr: str) -> Outcome:
    """Yosys's verdict: no dump is a preprocessor error; a backtick word left in the dump is the
    lexer's "undefined macro" error; otherwise the dump's tokens (a later parse error does not
    concern the preprocessor)."""
    dump = yosys_dump(log)
    if dump is None:
        return Outcome.rejected(_error_line(stderr + log, "ERROR"))
    tokens = tokenize(normalize(dump))
    left = [t for t in tokens if t.startswith("`")]
    if left:
        return Outcome.rejected(f"undefined macro {left[0]} (left in the dump)")
    return Outcome.accepted(tokens)


def run_yosys(opts: Options, source: Path, work: Path) -> Outcome:
    args = ["-nosynthesis", "-ppdump"]
    args += [f"-I{d}" for d in opts.incdirs] + [f"-D{d}" for d in opts.defines]
    script = "read_verilog " + " ".join(_word(a) for a in [*args, str(source)])
    _, log, stderr = _run([opts.yosys, "-Q", "-T", "-p", script], work)
    return yosys_outcome(log, stderr)


# ---- input set ---------------------------------------------------------------------------------

_INCLUDE = re.compile(r'^[^\n]*?`include\s*"([^"\n]+)"', re.MULTILINE)


def _includes(path: Path, text: str) -> list[Path]:
    found = []
    for match in _INCLUDE.finditer(text):
        if "//" in match.group(0):  # commented out (arm_core.v's `include lines)
            continue
        found.append(Path(os.path.normpath(path.parent / match.group(1))))
    return found


def backtick_roots(root: Path) -> tuple[list[Path], dict[Path, Path]]:
    """The .v/.vh files under ``root`` holding a backtick, split into roots (sorted) and the
    files another one includes (each mapped to an includer: they are compared through it)."""
    files = sorted(p for p in root.rglob("*") if p.suffix in (".v", ".vh") and p.is_file())
    texts = {p: p.read_text(errors="replace") for p in files}
    covered: dict[Path, Path] = {}
    for path in files:
        for included in _includes(path, texts[path]):
            covered.setdefault(included, path)
    roots = [p for p in files if "`" in texts[p] and p not in covered]
    covered = {p: by for p, by in covered.items() if p in texts and "`" in texts[p]}
    return roots, covered


def smallest_first(paths: Sequence[Path]) -> list[Path]:
    """Ascending size (then name), any mcml.v last."""
    return sorted(paths, key=lambda p: (p.name == LAST, p.stat().st_size, str(p)))


def _suffix_key(path: Path, table: dict[str, object]) -> str | None:
    text = path.as_posix()
    return next((key for key in table if text.endswith("/" + key) or text == key), None)


def suite(repo: Path, vtr: Path) -> tuple[list[Path], list[tuple[Path, str]]]:
    """The standard input set and the files left out of it, each with its reason."""
    excluded: list[tuple[Path, str]] = []
    files: list[Path] = []
    for path in sorted((repo / PROBES).glob("*.v")) + sorted((repo / FIXTURES).glob("*.v")):
        key = _suffix_key(path, dict(EXCLUDED))
        if key is not None:
            excluded.append((path, EXCLUDED[key]))
        else:
            files.append(path)
    roots, covered = backtick_roots(repo / MICROS)
    files += roots
    for path, by in sorted(covered.items()):
        excluded.append((path, f"included by {by.relative_to(repo)}; compared through it"))
    files += sorted(vtr.glob("*.v"))
    return smallest_first(files), excluded


# ---- driver ------------------------------------------------------------------------------------


def check_file(opts: Options, source: Path, scratch: Path) -> tuple[Kind, str]:
    """Run the three tools on ``source`` from an empty directory and classify the result."""
    with tempfile.TemporaryDirectory(dir=scratch) as tmp:
        work = Path(tmp)
        odin3 = run_odin3(opts, source, work)
        icarus = run_icarus(opts, source, work)
        yosys = run_yosys(opts, source, work)
    key = _suffix_key(source, dict(DECLARED))
    return classify(odin3, icarus, yosys, DECLARED[key][0] if key is not None else None)


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


def _missing_tool(opts: Options) -> str | None:
    for tool in (opts.odin3, opts.iverilog, opts.yosys):
        if shutil.which(tool) is None:
            return tool
    return None


def _inputs(args: argparse.Namespace) -> tuple[list[Path], list[tuple[Path, str]]] | str:
    files = [p.resolve() for p in args.files]
    excluded: list[tuple[Path, str]] = []
    if args.suite is not None:
        vtr_root = os.environ.get("VTR_ROOT", str(DEFAULT_VTR_ROOT))
        vtr = args.vtr if args.vtr is not None else Path(vtr_root) / VTR_SUBDIR
        if not vtr.is_dir():
            return f"VTR benchmark directory {vtr}"
        more, excluded = suite(args.suite.resolve(), vtr.resolve())
        files += more
    return smallest_first(files), excluded


def _report(results: list[tuple[Path, Kind, str]], excluded: list[tuple[Path, str]]) -> int:
    counts = {kind: 0 for kind in Kind}
    for kind in (Kind.ORACLES_DISAGREE, Kind.DECLARED, Kind.DIFFER, Kind.DECLARED_UNMET):
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
    failed = counts[Kind.DIFFER] + counts[Kind.DECLARED_UNMET]
    return EXIT_DIFFER if failed else EXIT_OK


def main(argv: Sequence[str]) -> int:
    args = _parse(argv)
    opts = Options(
        args.odin3, args.iverilog, args.yosys, tuple(args.incdirs), tuple(args.defines)
    )
    missing = _missing_tool(opts)
    inputs = _inputs(args)
    if missing is not None or isinstance(inputs, str):
        print(f"pp-diff: skipped: {missing or inputs} not found", file=sys.stderr)
        return EXIT_SKIP
    files, excluded = inputs
    if not files:
        print("pp-diff: no input files", file=sys.stderr)
        return EXIT_USAGE
    results: list[tuple[Path, Kind, str]] = []
    with tempfile.TemporaryDirectory(prefix="pp-diff-", dir=args.work) as scratch:
        for path in files:
            kind, detail = check_file(opts, path, Path(scratch))
            print(f"{kind.value:16} {path} ({detail})", flush=True)
            results.append((path, kind, detail))
    return _report(results, excluded)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

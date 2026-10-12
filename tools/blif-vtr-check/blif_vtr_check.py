"""blif-vtr-check: the VPR-free checker for ``write_blif --vtr`` output (spec 3E §15.1, VB-15).

Usage::

    blif-vtr-check [--models FILE] [--lut-k K] [--strict] [--eblif] [--max-errors N] FILE...

It does not reuse ``tools/blif/blif.py``'s permissive reader. It carries its own lexer, which
mirrors libblifparse's ``blif_lexer.l`` at VTR ``3c9a4d23`` (states INITIAL, NAMES, LATCH and
SO_COVER; ``ID_SET`` is ``[^ \\t\\r\\n\\\\="]``; a ``#`` comment only where a token starts, by
longest match, so ``a#b`` is one name; ``\\`` + newline a continuation in every state), and the
checks of VPR's ``read_blif.cpp`` and ``read_circuit.cpp`` (rules R1-R16 of the spec), so it
rejects what VPR rejects:

- C1 illegal bytes and unterminated quoted strings (R1); C2 directives VPR does not read (R3);
- C3 models (R13); C4 ``.names`` wider than K (R4); C5 cover rows (R5); C5b one-input
  ``.names`` with other than one row (R5b); C6 ``.latch`` (R2, R6-R8);
- C7 ``.subckt`` models and formals (R9, R10); C8 block names (R12); C9 multi-driven nets (R16,
  ``.inputs`` pads count as drivers).

``--strict`` adds Odin III's own conventions (C-S1 to C-S5): no latch init 3, no ``unconn`` on a
``.names``/``.latch`` pin, ``gnd``/``vcc``/``unconn`` are the right constant generators, the
``write_blif --vtr`` header, no ``.clock``, no unused stub, and clock net names
``blackbox_latches.pl`` can split.

The models file is the JSON ``tools/vtr-models extract`` writes (spec §15.2); only ``lut_size``
and ``models`` are read.

Violations go to stdout, one per line, ``blif-vtr-check: <file>:<line>: <message>``, at most
``--max-errors`` per file (default 50). Exit 0 when every file is legal, 1 on any violation, 2
for usage, I/O or an unreadable models file.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections.abc import Iterator
from dataclasses import dataclass, field
from pathlib import Path

PROG = "blif-vtr-check"
REPO = Path(__file__).resolve().parents[2]
DEFAULT_MODELS = REPO / "tests" / "vtr" / "arch" / "k6_frac_N10_frac_chain_mem32K_40nm.models.json"
DEFAULT_MAX_ERRORS = 50

# Token kinds. EQ ('=' in the INITIAL state, a .subckt formal's separator) is the lexer's sixth
# kind: libblifparse returns it as its own token, and no name may contain it.
DIRECTIVE = "DIRECTIVE"
NAME = "NAME"
STRING = "STRING"
EOL = "EOL"
LATCH_KW = "LATCH_KW"
EQ = "EQ"

DIRECTIVES = frozenset(
    {".names", ".latch", ".model", ".subckt", ".inputs", ".outputs", ".end", ".blackbox",
     ".conn", ".attr", ".param", ".cname"}
)
EBLIF_DIRECTIVES = frozenset({".conn", ".attr", ".param", ".cname"})
LATCH_TYPES = frozenset({"re", "fe", "ah", "al", "as"})
LATCH_INITS = frozenset({"0", "1", "2", "3"})
LATCH_WORDS = LATCH_TYPES | LATCH_INITS | {"NIL"}
CONSTANT_VALUES = {"gnd": "0", "vcc": "1", "unconn": "0"}
COVER_CHARS = frozenset("01-")
HEADER_RE = r"^# Odin III \S+ write_blif --vtr \(K={k}, names (short|provenance)\)$"

_INITIAL, _NAMES, _LATCH, _SO_COVER = range(4)
_WORD_RE = re.compile(r'(?:[^ \t\r\n\\="]|\\)*[^ \t\r\n\\="]')
_QUOTED_RE = re.compile(r'"[^\r\n"]*"')
_COVER_RE = re.compile(r"[01\-]+")
_ENDL_RE = re.compile(r"\n\r|\r\n|\n")
_BLANK_RE = re.compile(r"[ \t]*(?:\n\r|\r\n|\n)")
_WS_RE = re.compile(r"[ \t]+")
_COMMENT_RE = re.compile(r"#[^\n]*\n\r?")
_CONT_BLANK_RE = re.compile(r"\\(?:\n\r|\r\n|\n)[ \t]*(?:\n\r|\r\n|\n)")
_INDEX_RE = re.compile(r"^(.*)\[(\d+)\]$")


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    line: int


class LexError(Exception):
    """An input byte libblifparse's lexer does not accept (its "Unrecognized character")."""

    def __init__(self, line: int, message: str) -> None:
        super().__init__(f"{line}: {message}")
        self.line = line
        self.message = message


@dataclass(frozen=True)
class ModelPorts:
    inputs: dict[str, int]
    outputs: dict[str, int]
    clocks: tuple[str, ...]


@dataclass(frozen=True)
class Models:
    lut_size: int
    models: dict[str, ModelPorts]


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    message: str


@dataclass(frozen=True)
class CheckOptions:
    lut_k: int
    strict: bool
    eblif: bool
    max_errors: int


class ModelsError(Exception):
    """The models JSON is missing a key or holds a value of the wrong type."""


def _byte(char: str) -> str:
    return f"<0x{ord(char):02X}>"


def _cover_char(char: str) -> str:
    shown = char if "\x21" <= char <= "\x7e" else _byte(char)
    return f"cover row character '{shown}'"


class _Lexer:
    """libblifparse's lexer, one rule per method; ``run`` yields the tokens."""

    def __init__(self, text: str) -> None:
        self.text = text
        self.pos = 0
        self.line = 1
        self.state = _INITIAL

    def _take(self, end: int) -> None:
        self.line += self.text.count("\n", self.pos, end)
        self.pos = end

    def run(self) -> Iterator[Token]:
        while self.pos < len(self.text):
            token = self._step()
            if token is not None:
                yield token
        yield Token(EOL, "", self.line)

    def _step(self) -> Token | None:
        text, pos = self.text, self.pos
        char = text[pos]
        if char == "#":
            comment = _COMMENT_RE.match(text, pos)
            if comment:
                return self._comment(comment.end())
        if char == "\\" and _ENDL_RE.match(text, pos + 1):
            return self._continuation()
        if self.state == _INITIAL and (pos == 0 or text[pos - 1] == "\n"):
            blank = _BLANK_RE.match(text, pos)
            if blank:  # ^{WS}*{ENDL}: a blank line, no EOL
                self._take(blank.end())
                return None
        if char in "\n\r":
            return self._endl()
        ws = _WS_RE.match(text, pos)
        if ws:
            self._take(ws.end())
            return None
        if self.state == _SO_COVER:
            return self._cover()
        return self._word()

    def _comment(self, end: int) -> Token:
        # <*>#.*{ENDL}: always longer than a word starting at '#'. Without an ENDL (the end
        # of the file) it does not match, and '#...' lexes as a word, as in flex. The EOL has
        # the text '#': a comment does not move NAMES to SO_COVER, so rows after it are names.
        line = self.line
        self._take(end)
        return Token(EOL, "#", line)

    def _continuation(self) -> Token | None:
        line = self.line
        if self.state == _INITIAL:
            match = _CONT_BLANK_RE.match(self.text, self.pos)
            if match:  # \{ENDL}{WS}*{ENDL}: a continuation into a blank line ends the line
                self._take(match.end())
                return Token(EOL, "", line)
        match = _ENDL_RE.match(self.text, self.pos + 1)
        assert match is not None
        self._take(match.end())
        return None

    def _endl(self) -> Token:
        line = self.line
        match = _ENDL_RE.match(self.text, self.pos)
        if match is None:
            raise LexError(line, f"illegal character {_byte(self.text[self.pos])} in name")
        self._take(match.end())
        if self.state == _NAMES:
            self.state = _SO_COVER
        elif self.state == _LATCH:
            self.state = _INITIAL
        return Token(EOL, "", line)

    def _directive(self, word: str) -> Token:
        if word == ".names":
            self.state = _NAMES
        elif word == ".latch":
            self.state = _LATCH
        else:
            self.state = _INITIAL
        return Token(DIRECTIVE, word, self.line)

    def _cover(self) -> Token:
        # SO_COVER: 0 1 - and the directives only. A word starting with '.' that is no
        # directive is returned whole and the rest of its line lexes as in INITIAL, so the
        # checker names it (C2); flex stops at the '.' ("Unrecognized character").
        text, pos, line = self.text, self.pos, self.line
        cover = _COVER_RE.match(text, pos)
        if cover:
            self._take(cover.end())
            return Token(NAME, cover.group(), line)
        word = _WORD_RE.match(text, pos)
        if text[pos] == "." and word:
            self._take(word.end())
            if word.group() in DIRECTIVES:
                return self._directive(word.group())
            self.state = _INITIAL
            return Token(NAME, word.group(), line)
        raise LexError(line, _cover_char(text[pos]))

    def _word(self) -> Token:
        text, pos, line = self.text, self.pos, self.line
        char = text[pos]
        if char == "=" and self.state == _INITIAL:
            self._take(pos + 1)
            return Token(EQ, "=", line)
        if char == '"':
            quoted = _QUOTED_RE.match(text, pos)
            if quoted is None:
                raise LexError(line, "unterminated quoted string")
            self._take(quoted.end())
            return Token(STRING, quoted.group(), line)
        word = _WORD_RE.match(text, pos)
        if word is None:
            raise LexError(line, f"illegal character {_byte(char)} in name")
        self._take(word.end())
        value = word.group()
        if value in DIRECTIVES:
            return self._directive(value)
        if self.state == _LATCH and value in LATCH_WORDS:
            return Token(LATCH_KW, value, line)
        return Token(NAME, value, line)


def lex(text: str) -> Iterator[Token]:
    """Tokens of ``text`` as libblifparse's lexer returns them, ending with one EOL.

    Raises ``LexError`` at the first byte the lexer rejects.
    """
    return _Lexer(text).run()


def _int_map(value: object, where: str) -> dict[str, int]:
    if not isinstance(value, dict):
        raise ModelsError(f"{where}: not an object")
    out: dict[str, int] = {}
    for key, width in value.items():
        if (not isinstance(key, str) or not isinstance(width, int) or isinstance(width, bool)
                or width < 1):
            raise ModelsError(f"{where}: port '{key}' needs a positive width")
        out[key] = width
    return out


def _model_ports(name: str, value: object) -> ModelPorts:
    if not isinstance(value, dict):
        raise ModelsError(f"models: {name}: not an object")
    clocks = value.get("clocks", [])
    if not isinstance(clocks, list) or not all(isinstance(c, str) for c in clocks):
        raise ModelsError(f"models: {name}: clocks: not a list of names")
    return ModelPorts(
        inputs=_int_map(value.get("inputs"), f"models: {name}: inputs"),
        outputs=_int_map(value.get("outputs"), f"models: {name}: outputs"),
        clocks=tuple(clocks),
    )


def load_models(path: Path) -> Models:
    """Read a models JSON (spec §15.2). Raises OSError, ValueError or ModelsError."""
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ModelsError("not a JSON object")
    lut_size = data.get("lut_size")
    if not isinstance(lut_size, int) or isinstance(lut_size, bool) or lut_size < 1:
        raise ModelsError("lut_size: missing or not a positive integer")
    models = data.get("models")
    if not isinstance(models, dict):
        raise ModelsError("models: missing")
    return Models(lut_size, {name: _model_ports(name, value) for name, value in models.items()})


def split_index(formal: str) -> tuple[str, int]:
    """VPR's ``split_index``: ``p[i]`` is bit i of p, anything else bit 0 of itself."""
    match = _INDEX_RE.match(formal)
    if match is None:
        return formal, 0
    return match.group(1), int(match.group(2))


@dataclass
class _Names:
    nets: list[Token]
    line: int
    rows: list[str] = field(default_factory=list)
    after_comment: bool = False


@dataclass
class _Subckt:
    model: str
    line: int
    formals: list[tuple[Token, Token]]


@dataclass
class _Model:
    name: str
    line: int
    blackbox_line: int = 0
    inputs: list[Token] = field(default_factory=list)
    outputs: list[Token] = field(default_factory=list)
    primitives: int = 0
    names: list[_Names] = field(default_factory=list)
    # (net, line) in statement order, for C9, C-S3
    drivers: list[tuple[str, int]] = field(default_factory=list)
    # (block name, kind, line) in statement order, for C8
    blocks: list[tuple[str, str, int]] = field(default_factory=list)
    # (net, line, what): what is "logic" (a .names input, a .latch D), "latch clock", "hard",
    # "hard clock" or "output"
    sinks: list[tuple[str, int, str]] = field(default_factory=list)
    subckts: list[_Subckt] = field(default_factory=list)
    latch_inits: list[tuple[str, int]] = field(default_factory=list)


class _Checker:
    """One file: statements from the tokens, then the model and net checks."""

    def __init__(self, models: Models, opts: CheckOptions) -> None:
        self.arch = models
        self.opts = opts
        self.found: list[tuple[int, str]] = []
        self.models: list[_Model] = []
        self.current: _Model | None = None
        self.names: _Names | None = None
        self.clock_lines: list[int] = []

    def report(self, line: int, message: str) -> None:
        self.found.append((line, message))

    # --- statements -------------------------------------------------------------------------

    def statements(self, tokens: list[Token], complete: bool) -> None:
        """Group ``tokens`` into lines. Without ``complete`` (a lexer error), the unfinished
        line is dropped, and the open .names is not judged (its rows stop at the error)."""
        stmt: list[Token] = []
        for token in tokens:
            if token.kind == EOL:
                self._statement(stmt)
                header = bool(stmt) and stmt[0].text == ".names"
                if token.text == "#" and header and self.names is not None:
                    self.names.after_comment = True  # the lexer stays in NAMES (no SO_COVER)
                stmt = []
            elif token.kind == DIRECTIVE and stmt:
                self.report(token.line, f"syntax: '{token.text}' must start a line")
                self._statement(stmt)
                stmt = [token]
            else:
                stmt.append(token)
        if not complete:
            self.names = None
        self._close_names()

    def _statement(self, stmt: list[Token]) -> None:
        if not stmt:  # a blank or comment line: in SO_COVER it ends the cover (EOL)
            self._close_names()
            return
        head = stmt[0]
        if head.kind == DIRECTIVE:
            self._close_names()
            self._directive(head, stmt[1:])
        elif self.names is not None and not head.text.startswith("."):
            self._row(stmt)
        else:
            self._close_names()
            self._stray(head)

    def _stray(self, head: Token) -> None:
        if head.kind == NAME and _COVER_RE.fullmatch(head.text):
            self.report(head.line, "syntax: cover row outside a .names "
                                   "(a blank or comment line ends the cover)")
            return
        self.report(head.line, f"directive '{head.text}' is not read by VPR")
        if head.text == ".clock":
            self.clock_lines.append(head.line)

    def _directive(self, head: Token, args: list[Token]) -> None:
        word, line = head.text, head.line
        if word in EBLIF_DIRECTIVES:
            if not self.opts.eblif:
                self.report(line, f"'{word}' needs --circuit_format eblif")
            return
        if word == ".model":
            self._model(head, args)
            return
        model = self.current
        if model is None:
            self.report(line, f"syntax: '{word}' outside a .model")
            return
        for token in args:
            if token.kind == EQ and word != ".subckt":
                self.report(token.line, "illegal character <0x3D> in name")
                return
        if word in (".inputs", ".outputs"):
            self._ports(model, word, args)
        elif word == ".names":
            self._open_names(model, head, args)
        elif word == ".latch":
            self._latch(model, line, args)
        elif word == ".subckt":
            self._subckt(model, line, args)
        else:
            self._bare(model, head, args)

    def _bare(self, model: _Model, head: Token, args: list[Token]) -> None:
        if args:
            self.report(head.line, f"syntax: '{head.text}' takes no arguments")
        if head.text == ".blackbox":
            model.blackbox_line = head.line
        else:  # .end
            self.current = None

    def _model(self, head: Token, args: list[Token]) -> None:
        if len(args) != 1 or args[0].kind not in (NAME, STRING):
            self.report(head.line, "syntax: '.model' takes one name")
        name = args[0].text if args else ""
        self.current = _Model(name, head.line)
        self.models.append(self.current)

    def _ports(self, model: _Model, word: str, args: list[Token]) -> None:
        for token in args:
            if word == ".inputs":
                model.inputs.append(token)
                model.drivers.append((token.text, token.line))
                model.blocks.append((token.text, "input", token.line))
            else:
                model.outputs.append(token)
                model.sinks.append((token.text, token.line, "output"))
                model.blocks.append(("out:" + token.text, "output", token.line))

    # --- .names (C4, C5, C5b) -----------------------------------------------------------------

    def _open_names(self, model: _Model, head: Token, args: list[Token]) -> None:
        model.primitives += 1
        if not args:
            self.report(head.line, "syntax: '.names' has no nets")
            return
        self.names = _Names(args, head.line)
        model.names.append(self.names)
        inputs = len(args) - 1
        if inputs > self.opts.lut_k:
            self.report(head.line, f".names with {inputs} inputs exceeds K={self.opts.lut_k}")
        output = args[-1]
        model.drivers.append((output.text, output.line))
        model.blocks.append((output.text, "names", head.line))
        for net in args[:-1]:
            model.sinks.append((net.text, net.line, "logic"))

    def _row(self, stmt: list[Token]) -> None:
        names = self.names
        assert names is not None
        line = stmt[0].line
        if names.after_comment and not names.rows:
            self.report(line, "syntax: cover row after a comment on its .names line "
                              "(VPR reads the row as names)")
        row = "".join(token.text for token in stmt)
        bad = next((c for c in row if c not in COVER_CHARS), None)
        if bad is not None:
            self.report(line, _cover_char(bad))
        elif len(row) != len(names.nets):
            self.report(line, f"cover row has {len(row) - 1} columns, "
                              f"expected {len(names.nets) - 1}")
        names.rows.append(row)

    def _close_names(self) -> None:
        names = self.names
        self.names = None
        if names is None or len(names.nets) != 2 or len(names.rows) == 1:
            return
        output = names.nets[-1].text
        self.report(names.line, f"1-input .names '{output}' has {len(names.rows)} cover rows; "
                                "VPR asserts exactly one (absorb_buffer_luts)")

    # --- .latch (C6) ----------------------------------------------------------------------------

    def _latch(self, model: _Model, line: int, args: list[Token]) -> None:
        model.primitives += 1
        if len(args) < 2 or len(args) > 5:
            self.report(line, "syntax: '.latch' takes 2 to 5 fields")
            return
        problem = self._latch_problem(args)
        data, out = args[0], args[1]
        if data.kind != LATCH_KW:
            model.sinks.append((data.text, line, "logic"))
        if out.kind != LATCH_KW:
            model.drivers.append((out.text, line))
            model.blocks.append((out.text, "latch", line))
        if problem is not None:
            self.report(line, problem)
            return
        model.sinks.append((args[3].text, line, "latch clock"))
        if len(args) == 5:
            model.latch_inits.append((args[4].text, line))

    @staticmethod
    def _latch_problem(args: list[Token]) -> str | None:
        """The first VPR error on a .latch line: parse errors, then type, then clock."""
        rest = args[2:]
        control = [rest[1]] if len(rest) >= 2 and rest[1].text != "NIL" else []
        for token in [args[0], args[1], *control]:
            if token.kind == LATCH_KW:
                return f"'{token.text}' on a .latch line lexes as a keyword"
        if len(rest) == 1 and rest[0].kind != LATCH_KW:
            return f"latch init '{rest[0].text}'"
        if len(rest) == 3 and (rest[2].kind != LATCH_KW or rest[2].text not in LATCH_INITS):
            return f"latch init '{rest[2].text}'"
        if len(rest) >= 2 and (rest[0].kind != LATCH_KW or rest[0].text != "re"):
            return f"latch type '{rest[0].text}' (VPR accepts only re)"
        if len(rest) < 2 or rest[1].kind == LATCH_KW:
            return "latch without a clock"
        return None

    # --- .subckt (C7) ---------------------------------------------------------------------------

    def _subckt(self, model: _Model, line: int, args: list[Token]) -> None:
        model.primitives += 1
        formals = self._formals(line, args)
        if formals is None:
            return
        name = args[0].text
        model.subckts.append(_Subckt(name, line, formals))
        ports = self.arch.models.get(name)
        if ports is None:
            self.report(line, f".subckt model '{name}' is not an arch model")
            return
        seen: set[tuple[str, int]] = set()
        block = ""
        for formal, net in formals:
            base, bit = split_index(formal.text)
            width = ports.inputs.get(base, ports.outputs.get(base))
            if width is None:
                self.report(line, f"no port '{formal.text}' on model '{name}'")
                continue
            if bit >= width:
                self.report(line, f"port '{formal.text}' exceeds width {width} of '{name}'")
                continue
            if (base, bit) in seen:
                self.report(line, f"formal '{formal.text}' given twice")
                continue
            seen.add((base, bit))
            if base in ports.inputs:
                what = "hard clock" if base in ports.clocks else "hard"
                model.sinks.append((net.text, line, what))
                continue
            model.drivers.append((net.text, line))
            block = block or net.text
        if block:
            model.blocks.append((block, "subckt", line))

    def _formals(self, line: int, args: list[Token]) -> list[tuple[Token, Token]] | None:
        names = (NAME, STRING)
        if not args or args[0].kind not in names or (len(args) - 1) % 3 != 0:
            self.report(line, "syntax: '.subckt' takes a model and formal=net pairs")
            return None
        formals = []
        for i in range(1, len(args), 3):
            formal, eq, net = args[i : i + 3]
            if formal.kind not in names or eq.kind != EQ or net.kind not in names:
                self.report(line, "syntax: '.subckt' takes a model and formal=net pairs")
                return None
            formals.append((formal, net))
        return formals

    # --- models (C3) and nets (C8, C9) ------------------------------------------------------------

    def model_checks(self, complete: bool) -> _Model | None:
        top: _Model | None = None
        for model in self.models:
            if model.blackbox_line:
                self._stub(model)
            elif top is None:
                top = model
            else:
                self.report(model.line, f"second model '{model.name}' with primitives; "
                                        "VPR reads one")
            self._nets(model)
        if top is None and complete:
            self.report(1, "no model with primitives; VPR needs one")
        return top

    def _stub(self, model: _Model) -> None:
        ports = self.arch.models.get(model.name)
        if ports is None:
            self.report(model.blackbox_line, f"black-box model '{model.name}' is not an arch model")
            return
        if model.primitives:
            self.report(model.blackbox_line, f"black-box model '{model.name}' has primitives")
        for tokens, own, other in ((model.inputs, ports.inputs, ports.outputs),
                                   (model.outputs, ports.outputs, ports.inputs)):
            self._stub_ports(model.name, tokens, own, other)

    def _stub_ports(self, name: str, tokens: list[Token], own: dict[str, int],
                    other: dict[str, int]) -> None:
        widest: dict[str, Token] = {}
        for token in tokens:
            base, bit = split_index(token.text)
            if base not in widest or split_index(widest[base].text)[1] < bit:
                widest[base] = token
        for base, token in widest.items():
            width = split_index(token.text)[1] + 1
            if base in other:
                reason = "direction differs from the model"
            elif base not in own:
                reason = "no such port on the model"
            elif width > own[base]:
                reason = f"width {width} exceeds {own[base]}"
            else:
                continue
            self.report(token.line, f"stub port '{base}' of '{name}': {reason}")

    def _nets(self, model: _Model) -> None:
        kinds: dict[str, str] = {}
        for block, kind, line in model.blocks:
            if block in kinds:
                self.report(line, f"duplicate block name '{block}' ({kinds[block]} and {kind})")
            else:
                kinds[block] = kind
        count: dict[str, int] = {}
        second: dict[str, int] = {}
        for net, line in model.drivers:
            count[net] = count.get(net, 0) + 1
            if count[net] == 2:
                second[net] = line
        for net, line in second.items():
            self.report(line, f"net '{net}' has {count[net]} drivers")

    # --- strict (C-S1 to C-S5) ------------------------------------------------------------------

    def strict(self, text: str, top: _Model | None, complete: bool) -> None:
        """C-S1 to C-S5. Without ``complete`` (a lexer error), the rules that judge the
        whole file (C-S3's generators, C-S4's unused stubs) are skipped."""
        first = text.split("\n", 1)[0].rstrip("\r")
        if not re.match(HEADER_RE.format(k=self.opts.lut_k), first):
            self.report(1, "missing header")
        for line in self.clock_lines:
            self.report(line, ".clock present")
        if top is None:
            return
        for value, line in top.latch_inits:
            if value == "3":
                self.report(line, "latch init 3 (write 2; VTR's ABC rejects 3)")
        unconn_lines = sorted({line for net, line, what in top.sinks
                               if net == "unconn" and what in ("logic", "latch clock")})
        for line in unconn_lines:
            self.report(line, "net 'unconn' feeds a .names/.latch pin (VPR leaves it open)")
        if complete:
            self._constants(top)
            self._unused_stubs(top)
        for net, line, what in top.sinks:
            if what.endswith("clock") and ("|" in net or "_^_" in net):
                self.report(line, f"clock net '{net}' contains '|' or '_^_' "
                                  "(breaks VTR's blackbox_latches.pl)")

    def _unused_stubs(self, top: _Model) -> None:
        used = {subckt.model for subckt in top.subckts}
        for model in self.models:
            if model.blackbox_line and model.name not in used:
                self.report(model.line, f"stub '{model.name}' unused")

    def _constants(self, top: _Model) -> None:
        generators = {names.nets[-1].text: names for names in top.names if names.nets}
        for name, value in CONSTANT_VALUES.items():
            drivers = [line for net, line in top.drivers if net == name]
            uses = [line for net, line, _ in top.sinks if net == name]
            if not drivers and not uses:
                continue
            names = generators.get(name)
            if names is not None and len(names.nets) == 1 and _constant(names) == value:
                continue
            line = drivers[0] if drivers else uses[0]
            self.report(line, f"'{name}' is not a constant generator of the right value")


def _constant(names: _Names) -> str:
    """The value of a zero-input .names: no rows (or rows of 0) 0, a row 1 makes it 1."""
    return "1" if any(row == "1" for row in names.rows) else "0"


def check_text(text: str, path: str, models: Models, opts: CheckOptions) -> list[Violation]:
    """Every violation in ``text`` (C1-C9, C5b; C-S1-C-S5 with ``strict``), in line order, at
    most ``opts.max_errors``. A lexer error ends the file, as it ends VPR's parse."""
    tokens: list[Token] = []
    lex_error: LexError | None = None
    try:
        for token in lex(text):
            tokens.append(token)
    except LexError as err:
        lex_error = err
    checker = _Checker(models, opts)
    checker.statements(tokens, lex_error is None)
    top = checker.model_checks(lex_error is None)
    if opts.strict:
        checker.strict(text, top, lex_error is None)
    found = sorted(checker.found, key=lambda item: item[0])
    if lex_error is not None:
        found = [item for item in found if item[0] < lex_error.line]
        found.append((lex_error.line, lex_error.message))
    return [Violation(path, line, message) for line, message in found[: opts.max_errors]]


def _positive(value: str) -> int:
    try:
        number = int(value)
    except ValueError:
        number = 0
    if number < 1:
        raise argparse.ArgumentTypeError(f"expects a positive integer, got '{value}'")
    return number


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog=PROG, description="Check BLIF files as VPR reads them.")
    parser.add_argument("--models", type=Path, default=DEFAULT_MODELS,
                        help="the arch's models JSON (tools/vtr-models extract)")
    parser.add_argument("--lut-k", type=_positive, help="LUT size K (default: the JSON's)")
    parser.add_argument("--strict", action="store_true", help="also Odin III's conventions")
    parser.add_argument("--eblif", action="store_true", help="accept .conn .attr .param .cname")
    parser.add_argument("--max-errors", type=_positive, default=DEFAULT_MAX_ERRORS,
                        help="violations shown per file (default 50)")
    parser.add_argument("files", nargs="+", metavar="FILE")
    return parser


def main(argv: list[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
    except SystemExit as exc:
        return exc.code if isinstance(exc.code, int) else 2
    try:
        models = load_models(args.models)
    except (OSError, ValueError, ModelsError) as err:
        print(f"{PROG}: {args.models}: cannot read models: {err}", file=sys.stderr)
        return 2
    opts = CheckOptions(args.lut_k or models.lut_size, args.strict, args.eblif, args.max_errors)
    status = 0
    for name in args.files:
        try:
            text = Path(name).read_bytes().decode("latin-1")
        except OSError as err:
            print(f"{PROG}: {name}: cannot read: {err.strerror or err}", file=sys.stderr)
            status = 2
            continue
        for violation in check_text(text, name, models, opts):
            print(f"{PROG}: {violation.path}:{violation.line}: {violation.message}")
            status = max(status, 1)
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

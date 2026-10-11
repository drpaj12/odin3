#!/usr/bin/env python3
"""Machine-check the spec §15.1 C rules that no compiler flag or linter covers.

- exit()/_exit()/_Exit()/quick_exit() only under src/cli/ (the bare name is matched, so function
  pointers and macro aliases are caught too);
- abort(), raise(SIGABRT) and __builtin_trap() only in src/ir/check*.c (the debug-build `check`);
  assert() is allowed anywhere: it compiles out of release builds (decision 2026-10-08, #6);
- goto only to the single `cleanup` label.

Bison (.y) and Flex (.l) sources compile inside generated translation units that the rest of the
gate never sees (2C spec §6), so three more rules keep hand-written logic out of them:

- y-action: every .y action is exactly one VP(...) or VP_ERR(...) statement (actions.h);
- l-action: every .l action is one call into lexer_support.c (an `odin3_vl_*` function, its
  value optionally returned) beside Flex's own BEGIN(...) and yyless(...) macros, which
  lexer_support.c cannot call; `|` (the next rule's action) is allowed;
- action-lines: no .y or .l action spans more than three lines;
- prologue: %{ %}, %code and %top blocks hold only #include lines and the YY_USER_ACTION,
  YYLLOC_DEFAULT, YYMAXDEPTH, YYINITDEPTH and YYSTACK_USE_ALLOCA macros, and the code section
  after the second %% is empty.

VLAs are rejected by -Wvla and recursion by clang-tidy misc-no-recursion. Token-pasting tricks
are caught at link level by tools/check-symbols.sh (a CTest test). Comments and string
literals are ignored (in .y/.l files too). Usage: check_rules.py FILE...  Exit 0 clean, 1 on
violations.
"""

from __future__ import annotations

import re
import sys
from pathlib import PurePosixPath

_TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL
)
_EXIT = re.compile(r"\b(?:exit|_exit|_Exit|quick_exit)\b")
_ABORT = re.compile(r"\babort\b|\b__builtin_trap\b|\braise\s*\(\s*SIGABRT\b")
_GOTO = re.compile(r"\bgoto\s+(\w+)")


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and literals, keeping newlines so line numbers survive."""
    return _TOKENS.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def violations(path: str, text: str) -> list[str]:
    """Return 'path:line: message' for every rule violation in one file."""
    found = _c_violations(path, text)
    if path.endswith((".y", ".l")):
        found += grammar_violations(path, text)
    return found


def _c_violations(path: str, text: str) -> list[str]:
    """The exit/abort/goto rules (every file)."""
    posix = PurePosixPath(path)
    in_cli = posix.parts[:2] == ("src", "cli")
    is_check = posix.parts[:2] == ("src", "ir") and posix.name.startswith("check")
    found: list[str] = []
    for lineno, line in enumerate(strip_comments_and_strings(text).splitlines(), 1):
        if _EXIT.search(line) and not in_cli:
            found.append(f"{path}:{lineno}: exit() is only allowed under src/cli/")
        if _ABORT.search(line) and not is_check:
            found.append(f"{path}:{lineno}: abort/trap is only allowed in src/ir/check*.c")
        for match in _GOTO.finditer(line):
            if match.group(1) != "cleanup":
                found.append(f"{path}:{lineno}: goto may only target the 'cleanup' label")
    return found


_ACTION_LINES = 3
_ALLOWED_DEFINES = (
    "YY_USER_ACTION",
    "YYLLOC_DEFAULT",
    "YYMAXDEPTH",
    "YYINITDEPTH",
    "YYSTACK_USE_ALLOCA",
)
_PROLOGUE_LINE = re.compile(r"\s*(?:#\s*include\b.*|#\s*define\s+(\w+)\b.*|)$")
_BLOCK_OPEN = re.compile(r"%\{|%(?:code(?:\s+\w+)?|top)\s*\{")
_SECTION = re.compile(r"^%%", re.MULTILINE)
_Y_ACTION = re.compile(r"\s*(?:VP|VP_ERR)\s*\(.*\)\s*;\s*", re.DOTALL)
_L_STATEMENT = re.compile(r"\s*(?:return\s+)?(\w+)\s*\(.*\)\s*", re.DOTALL)
_FLEX_MACROS = ("BEGIN", "yyless")


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _brace_end(blank: str, start: int) -> int:
    """The offset just past the brace that closes the one at start (len(blank) if none)."""
    depth = 0
    for i in range(start, len(blank)):
        if blank[i] == "{":
            depth += 1
        elif blank[i] == "}":
            depth -= 1
            if depth == 0:
                return i + 1
    return len(blank)


def _prologue_violations(text: str, blank: str, end: int) -> list[tuple[int, str]]:
    """%{ %}, %code and %top blocks before offset end: only #include and the allowed macros."""
    found: list[tuple[int, str]] = []
    pos = 0
    while (match := _BLOCK_OPEN.search(blank, pos, end)) is not None:
        if match.group(0) == "%{":
            close = blank.find("%}", match.end())
            close = len(blank) if close < 0 else close
            body_end, pos = close, close + 2
        else:
            pos = _brace_end(blank, match.end() - 1)
            body_end = pos - 1
        for line in blank[match.end() : body_end].split("\n"):
            ok = _PROLOGUE_LINE.match(line)
            if ok is None or (ok.group(1) is not None and ok.group(1) not in _ALLOWED_DEFINES):
                found.append((_line_of(text, match.start()), "prologue"))
                break
    return found


def _pattern_end(line: str) -> int:
    """The end of a Flex rule's pattern: the first blank outside quotes, classes and braces."""
    i, quoted, in_class, braces = 0, False, False, 0
    while i < len(line):
        char = line[i]
        if char == "\\":
            i += 2
            continue
        if quoted:
            quoted = char != '"'
        elif in_class:
            in_class = char != "]"
        elif char == '"':
            quoted = True
        elif char == "[":
            in_class = True
            i += 2 if line[i + 1 : i + 2] in ("]", "^") else 1
            continue
        elif char in "{}":
            braces += 1 if char == "{" else -1
        elif char.isspace() and braces == 0:
            return i
        i += 1
    return i


def _y_action_ok(body: str) -> bool:
    """Exactly one VP(...) or VP_ERR(...) statement."""
    inner = body.strip().removeprefix("{").removesuffix("}")
    return inner.count(";") == 1 and _Y_ACTION.fullmatch(inner) is not None


def _l_action_ok(body: str) -> bool:
    """One odin3_vl_* call (optionally returned) beside BEGIN/yyless statements."""
    *statements, rest = body.strip().removeprefix("{").removesuffix("}").split(";")
    calls = 0
    for statement in statements:
        match = _L_STATEMENT.fullmatch(statement)
        if match is None:
            return False
        if match.group(1) not in _FLEX_MACROS:
            calls += 1 if match.group(1).startswith("odin3_vl_") else 2
    return calls == 1 and rest.strip() == ""


def _actions(blank: str, start: int, end: int, lexer: bool) -> list[tuple[int, int]]:
    """(start, end) offsets of every action in the rules section [start, end)."""
    found: list[tuple[int, int]] = []
    pos = start
    while pos < end:
        if lexer:
            line_end = blank.find("\n", pos, end)
            line_end = end if line_end < 0 else line_end
            line = blank[pos:line_end]
            at = pos + _pattern_end(line) if line[:1].strip() else line_end
            at += len(blank[at:line_end]) - len(blank[at:line_end].lstrip(" \t"))
            if at < line_end and blank[at] != "|":
                stop = _brace_end(blank, at) if blank[at] == "{" else line_end
                found.append((at, stop))
                line_end = max(line_end, stop)
            pos = line_end + 1
            continue
        brace = blank.find("{", pos, end)
        if brace < 0:
            break
        stop = _brace_end(blank, brace)
        found.append((brace, stop))
        pos = stop
    return found


def grammar_violations(path: str, text: str) -> list[str]:
    """The .y/.l hygiene rules (2C spec §6) for one Bison or Flex file."""
    lexer = path.endswith(".l")
    blank = strip_comments_and_strings(text)
    sections = [m.start() for m in _SECTION.finditer(blank)]
    rules_start = sections[0] + 2 if sections else len(blank)
    rules_end = sections[1] if len(sections) > 1 else len(blank)
    found = _prologue_violations(text, blank, rules_start)
    if blank[rules_end + 2 :].strip() if len(sections) > 1 else False:
        found.append((_line_of(text, rules_end), "prologue"))
    for start, stop in _actions(blank, rules_start, rules_end, lexer):
        body = blank[start:stop]
        if body.count("\n") >= _ACTION_LINES:
            found.append((_line_of(text, start), "action-lines"))
        elif lexer and not _l_action_ok(body):
            found.append((_line_of(text, start), "l-action"))
        elif not lexer and not _y_action_ok(body):
            found.append((_line_of(text, start), "y-action"))
    messages = {
        "prologue": "a prologue holds only #include lines and the YY* macros of 2C spec §6",
        "action-lines": "an action spans more than three lines",
        "l-action": "a .l action is one odin3_vl_* call (beside BEGIN/yyless)",
        "y-action": "a .y action is exactly one VP(...) or VP_ERR(...) statement",
    }
    return [f"{path}:{line}: {rule}: {messages[rule]}" for line, rule in found]


def main(argv: list[str]) -> int:
    found: list[str] = []
    for path in argv:
        with open(path, encoding="utf-8") as handle:
            found += violations(path, handle.read())
    for line in found:
        print(line)
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Machine-check the spec §15.1 C rules that no compiler flag or linter covers.

- exit()/_exit()/_Exit()/quick_exit() only under src/cli/ (the bare name is matched, so function
  pointers and macro aliases are caught too);
- abort(), raise(SIGABRT) and __builtin_trap() only in src/ir/check*.c (the debug-build `check`),
  with one exception: __builtin_trap() in the body of adp_offthread_stop in adapters/abc/guard.c,
  the linked-ABC adapter's off-thread stop (Peter's ruling (a), PHASE3 #28);
  assert() is allowed anywhere: it compiles out of release builds (decision 2026-10-08, #6),
  except in adapters/abc/: the adapter links with --wrap=__assert_fail, which would report its own
  failures as ABC's, so it uses ODIN3_ADAPTER_ASSERT instead (3D spec §4.1);
- goto only to the single `cleanup` label.

Bison (.y) and Flex (.l) sources compile inside generated translation units that the rest of the
gate never sees (2C spec §6), so more rules keep hand-written logic out of them:

- y-action: every .y action is exactly one VP(out, vp_*(...)) or VP_ERR(vp_*(...)) statement
  (actions.h): one call each, nothing after its closing parenthesis;
- l-action: every .l action is one call into lexer_support.c (an `odin3_vl_*` function, its
  value optionally returned) beside Flex's own BEGIN(name) and yyless(0) macros, which
  lexer_support.c cannot call, or exactly one BEGIN(name) statement and nothing else; `|` (the
  next rule's action) is allowed. A rule is a line starting in column 0; its pattern is read on
  the raw text (quotes, classes, braces);
- yyless: a scanner calls yyless at most once (the BASED exit, 2C spec §2.3);
- action-lines: no .y or .l action spans more than three lines;
- prologue: %{ %}, %code and %top blocks hold only #include lines and the YY_USER_ACTION,
  YYLLOC_DEFAULT, YYMAXDEPTH, YYINITDEPTH and YYSTACK_USE_ALLOCA macros, and the code section
  after the second %% is empty;
- code-block: no other .y block holds code: %initial-action, %destructor, %printer and any
  brace outside a directive are rejected, %union holds member declarations only, and
  %define/%param/%parse-param/%lex-param hold a plain value or declaration;
- verbatim: no indented .l line (Flex copies it into the scanner) other than a comment.

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
_ASSERT = re.compile(r"\bassert\s*\(")
# PHASE3 #28 (Peter's ruling (a), 2026-10-12): the ABC adapter's off-thread stop is the one trap
# outside `check`. The allowance covers __builtin_trap only, and only in this function's body in
# this file; anywhere else in adapters/abc/ or src/, a trap is still refused.
_TRAP_FILE = ("adapters", "abc", "guard.c")
_TRAP_FUNCTION = re.compile(r"\badp_offthread_stop\s*\(\s*void\s*\)\s*\{")


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and literals, keeping newlines so line numbers survive."""
    return _TOKENS.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def violations(path: str, text: str) -> list[str]:
    """Return 'path:line: message' for every rule violation in one file."""
    found = _c_violations(path, text)
    if path.endswith((".y", ".l")):
        found += grammar_violations(path, text)
    return found


def _trap_lines(posix: PurePosixPath, code: str) -> set[int]:
    """The lines of adp_offthread_stop's body in adapters/abc/guard.c (PHASE3 #28), else none."""
    match = _TRAP_FUNCTION.search(code) if posix.parts == _TRAP_FILE else None
    if match is None:
        return set()
    depth = 0
    for end in range(match.end() - 1, len(code)):
        depth += {"{": 1, "}": -1}.get(code[end], 0)
        if depth == 0:
            break
    first = code.count("\n", 0, match.start()) + 1
    return set(range(first, code.count("\n", 0, end) + 2))


def _abort_refused(line: str, lineno: int, trap_lines: set[int]) -> bool:
    """An abort/trap match on a line that the PHASE3 #28 allowance does not cover."""
    calls = _ABORT.findall(line)
    return bool(calls) and not (lineno in trap_lines and set(calls) == {"__builtin_trap"})


def _c_violations(path: str, text: str) -> list[str]:
    """The exit/abort/goto rules (every file) and the assert rule (adapters/abc/)."""
    posix = PurePosixPath(path)
    in_cli = posix.parts[:2] == ("src", "cli")
    is_check = posix.parts[:2] == ("src", "ir") and posix.name.startswith("check")
    in_abc_adapter = posix.parts[:2] == ("adapters", "abc")
    code = strip_comments_and_strings(text)
    trap_lines = _trap_lines(posix, code)
    found: list[str] = []
    for lineno, line in enumerate(code.splitlines(), 1):
        if _EXIT.search(line) and not in_cli:
            found.append(f"{path}:{lineno}: exit() is only allowed under src/cli/")
        if _abort_refused(line, lineno, trap_lines) and not is_check:
            found.append(f"{path}:{lineno}: abort/trap is only allowed in src/ir/check*.c")
        if _ASSERT.search(line) and in_abc_adapter:
            found.append(f"{path}:{lineno}: use ODIN3_ADAPTER_ASSERT, not assert() (3D §4.1)")
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
_SECTION = re.compile(r"^%%", re.MULTILINE)
_DIRECTIVE = re.compile(r"%([\w-]+)")
_BETWEEN = re.compile(r"[\w\s.\-<>]*")  # a directive's words before its brace
_PLAIN_DIRECTIVES = ("define", "parse-param", "lex-param", "param")
_PLAIN_BODY = re.compile(r"[\w\s*.]*")  # a type, a name, a value
_UNION_MEMBER = re.compile(r"\s*\w+(?:\s+\w+)*[\s*]+\w+\s*")
_CALL_HEAD = re.compile(r"\s*(return\s+)?(\w+)\s*\(")
_FLEX_ARG = {"BEGIN": re.compile(r"\s*\w+\s*"), "yyless": re.compile(r"\s*0\s*")}
_YYLESS = re.compile(r"\byyless\b")
_CLOSERS = {"(": ")", "[": "]", "{": "}"}
_MESSAGES = {
    "prologue": "a prologue holds only #include lines and the YY* macros of 2C spec §6, and "
    "the code section after the second %% is empty",
    "code-block": "a code block other than %{ %}/%code (%initial-action, %destructor, "
    "%printer, …) or a %union holding more than member declarations",
    "verbatim": "an indented .l line is code copied into the scanner",
    "action-lines": "an action spans more than three lines",
    "l-action": "a .l action is one odin3_vl_* call (beside BEGIN(name)/yyless(0)), or one "
    "BEGIN(name) alone",
    "yyless": "yyless is allowed once per scanner (the BASED exit of 2C spec §2.3)",
    "y-action": "a .y action is exactly one VP(out, vp_*(...)) or VP_ERR(vp_*(...)) statement",
}

Finding = tuple[int, str]  # (offset in the file, rule)


def _line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _matching(text: str, start: int) -> int:
    """The offset just past the bracket closing the one at start (len(text) if none)."""
    stack: list[str] = []
    for i in range(start, len(text)):
        char = text[i]
        if char in _CLOSERS:
            stack.append(_CLOSERS[char])
        elif stack and char == stack[-1]:
            stack.pop()
            if not stack:
                return i + 1
    return len(text)


def _split_top(text: str, sep: str) -> list[str]:
    """text split at sep outside every bracket."""
    parts, depth, last = [], 0, 0
    for i, char in enumerate(text):
        depth += 1 if char in _CLOSERS else -1 if char in ")]}" else 0
        if char == sep and depth == 0:
            parts.append(text[last:i])
            last = i + 1
    parts.append(text[last:])
    return parts


def _call(stmt: str) -> tuple[str, str, bool] | None:
    """(name, arguments, returned) when stmt is exactly `[return] name(arguments)`."""
    head = _CALL_HEAD.match(stmt)
    if head is None:
        return None
    close = _matching(stmt, head.end() - 1)
    if stmt[close:].strip() or close > len(stmt) or stmt[close - 1 : close] != ")":
        return None
    return head.group(2), stmt[head.end() : close - 1], head.group(1) is not None


def _is_vp_call(text: str) -> bool:
    call = _call(text)
    return call is not None and call[0].startswith("vp_") and not call[2]


def _y_action_ok(body: str) -> bool:
    """Exactly one VP(out, vp_*(...)) or VP_ERR(vp_*(...)) statement."""
    statements = _split_top(body, ";")
    call = _call(statements[0]) if len(statements) == 2 else None
    if call is None or call[2] or statements[1].strip():
        return False
    args = _split_top(call[1], ",")
    if call[0] == "VP":
        return len(args) == 2 and _is_vp_call(args[1])
    return call[0] == "VP_ERR" and len(args) == 1 and _is_vp_call(args[0])


def _l_action_ok(body: str) -> bool:
    """One odin3_vl_* call (optionally returned) beside BEGIN(name)/yyless(0) statements, or
    exactly one BEGIN(name) statement."""
    *statements, rest = _split_top(body, ";")
    calls, macros = 0, []
    for statement in statements:
        call = _call(statement)
        if call is None:
            return False
        name, args, returned = call
        if name in _FLEX_ARG and not returned and _FLEX_ARG[name].fullmatch(args):
            macros.append(name)
            continue
        if not name.startswith("odin3_vl_"):
            return False
        calls += 1
    begin_only = calls == 0 and macros == ["BEGIN"]
    return (calls == 1 or begin_only) and rest.strip() == ""


def _prologue_body(body: str, at: int) -> list[Finding]:
    """A %{ %} or %code/%top body: only #include and the allowed macros."""
    for line in body.split("\n"):
        ok = _PROLOGUE_LINE.match(line)
        if ok is None or (ok.group(1) is not None and ok.group(1) not in _ALLOWED_DEFINES):
            return [(at, "prologue")]
    return []


def _y_block(name: str, between: str, body: str, at: int) -> list[Finding]:
    """One braced block of a .y declarations section, opened after directive %name."""
    if not _BETWEEN.fullmatch(between):
        return [(at, "code-block")]  # code outside any directive
    if name in ("code", "top"):
        return _prologue_body(body, at)
    if name == "union":
        *members, rest = _split_top(body, ";")
        ok = rest.strip() == "" and all(_UNION_MEMBER.fullmatch(m) for m in members)
        return [] if ok else [(at, "code-block")]
    if name in _PLAIN_DIRECTIVES and _PLAIN_BODY.fullmatch(body):
        return []
    return [(at, "code-block")]


def _y_declarations(blank: str, end: int) -> list[Finding]:
    """Every braced block before the rules section: %{ %}, %code, %union, %define, … ."""
    found: list[Finding] = []
    directives = list(_DIRECTIVE.finditer(blank, 0, end))
    pos = 0
    while (brace := blank.find("{", pos, end)) >= 0:
        if blank[brace - 1 : brace] == "%":
            close = blank.find("%}", brace, end)
            close = end if close < 0 else close
            found += _prologue_body(blank[brace + 1 : close], brace - 1)
            pos = close + 2
            continue
        stop = _matching(blank, brace)
        owner = [d for d in directives if d.end() <= brace]
        name, after = (owner[-1].group(1), owner[-1].end()) if owner else ("", 0)
        found += _y_block(name, blank[after:brace], blank[brace + 1 : stop - 1], brace)
        pos = stop
    return found


def _y_findings(text: str) -> list[Finding]:
    blank = strip_comments_and_strings(text)
    sections = [m.start() for m in _SECTION.finditer(blank)] + [len(blank)] * 2
    found = _y_declarations(blank, sections[0])
    if blank[sections[1] + 2 :].strip():
        found.append((sections[1], "prologue"))
    pos = sections[0] + 2
    while (brace := blank.find("{", pos, sections[1])) >= 0:
        stop = _matching(blank, brace)
        if blank.count("\n", brace, stop) >= _ACTION_LINES:
            found.append((brace, "action-lines"))
        elif not _y_action_ok(blank[brace + 1 : stop - 1]):
            found.append((brace, "y-action"))
        pos = stop
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


def _l_rule(text: str, start: int, line: str) -> tuple[int, list[Finding]]:
    """One rule line of a .l rules section (raw text): where it ends, and its findings."""
    at = start + _pattern_end(line.rstrip("\n"))
    while at < len(text) and text[at] in " \t":
        at += 1
    line_end = start + len(line)
    if at >= line_end - 1 or text[at] == "|":
        tail = text[at + 1 : line_end] if text[at : at + 1] == "|" else ""
        return line_end, [] if not tail.strip() else [(at, "l-action")]
    window_end = at
    for _ in range(_ACTION_LINES + 1):  # an action is never longer
        nl = text.find("\n", window_end)
        window_end = len(text) if nl < 0 else nl + 1
    action = strip_comments_and_strings(text[at:window_end])
    if action[0] != "{":  # a bare statement up to the end of the line
        body = action[: line_end - at]
        return line_end, [] if _l_action_ok(body) else [(at, "l-action")]
    close = _matching(action, 0)
    nl = action.find("\n", close)
    stop = at + (len(action) if nl < 0 else nl + 1)
    if action.count("\n", 0, close) >= _ACTION_LINES or close == len(action):
        return stop, [(at, "action-lines")]
    trailing = action[close : stop - at].strip()
    ok = _l_action_ok(action[1 : close - 1]) and not trailing
    return stop, [] if ok else [(at, "l-action")]


def _l_block(text: str, blank: str, start: int) -> tuple[int, list[Finding]] | None:
    """A %{ %} or %top{ } block starting at start (column 0), or None."""
    if blank.startswith("%{", start):
        close = blank.find("%}", start)
        close = len(blank) if close < 0 else close
        return close + 2, _prologue_body(blank[start + 2 : close], start)
    if blank.startswith("%top", start) and blank[start + 4 :].lstrip(" \t").startswith("{"):
        brace = blank.index("{", start)
        stop = _matching(blank, brace)
        return stop, _prologue_body(blank[brace + 1 : stop - 1], start)
    return None


def _l_findings(text: str) -> list[Finding]:
    blank = strip_comments_and_strings(text)
    found: list[Finding] = [(m.start(), "yyless") for m in list(_YYLESS.finditer(blank))[1:]]
    section, offset, skip_to = 1, 0, 0
    for line in text.splitlines(keepends=True):
        start, offset = offset, offset + len(line)
        if start < skip_to:
            continue
        if line.startswith("%%"):
            section += 1
            continue
        code = blank[start:offset].strip()
        block = _l_block(text, blank, start)
        if block is not None:
            skip_to, more = block
            found += more
        elif section >= 3 or line[:1].isspace():
            found += [(start, "prologue" if section >= 3 else "verbatim")] if code else []
        elif section == 2 and code:
            skip_to, more = _l_rule(text, start, line)
            found += more
    return found


def grammar_violations(path: str, text: str) -> list[str]:
    """The .y/.l hygiene rules (2C spec §6) for one Bison or Flex file."""
    found = _l_findings(text) if path.endswith(".l") else _y_findings(text)
    return [
        f"{path}:{_line_of(text, at)}: {rule}: {_MESSAGES[rule]}" for at, rule in sorted(found)
    ]


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

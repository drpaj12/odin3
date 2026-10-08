#!/usr/bin/env python3
"""Machine-check the spec §15.1 C rules that no compiler flag or linter covers.

- exit()/_exit()/_Exit()/quick_exit() only under src/cli/ (the bare name is matched, so function
  pointers and macro aliases are caught too);
- abort() only in src/ir/check*.c (the debug-build `check`);
- goto only to the single `cleanup` label.

VLAs are rejected by -Wvla and recursion by clang-tidy misc-no-recursion. Token-pasting tricks
are caught at link level by tools/check-symbols.sh (a CTest test). Comments and string
literals are ignored. Usage: check_rules.py FILE...  Exit 0 clean, 1 on violations.
"""

from __future__ import annotations

import re
import sys
from pathlib import PurePosixPath

_TOKENS = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.DOTALL
)
_EXIT = re.compile(r"\b(?:exit|_exit|_Exit|quick_exit)\b")
_ABORT = re.compile(r"\babort\b")
_GOTO = re.compile(r"\bgoto\s+(\w+)")


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and literals, keeping newlines so line numbers survive."""
    return _TOKENS.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def violations(path: str, text: str) -> list[str]:
    """Return 'path:line: message' for every rule violation in one file."""
    posix = PurePosixPath(path)
    in_cli = posix.parts[:2] == ("src", "cli")
    is_check = posix.parts[:2] == ("src", "ir") and posix.name.startswith("check")
    found: list[str] = []
    for lineno, line in enumerate(strip_comments_and_strings(text).splitlines(), 1):
        if _EXIT.search(line) and not in_cli:
            found.append(f"{path}:{lineno}: exit() is only allowed under src/cli/")
        if _ABORT.search(line) and not is_check:
            found.append(f"{path}:{lineno}: abort() is only allowed in src/ir/check*.c")
        for match in _GOTO.finditer(line):
            if match.group(1) != "cleanup":
                found.append(f"{path}:{lineno}: goto may only target the 'cleanup' label")
    return found


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

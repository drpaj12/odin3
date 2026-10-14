"""toolutil: the small helpers every evidence tool needs (2F-6 review M3 and I1).

``exe`` (an executable as an absolute path), ``tail`` and ``why`` (a child's output in one line),
``parse_size`` (``--mem-cap`` values) and ``is_crash`` (the exit-code convention of the opt tools:
a child that died by a signal, was killed by its memory cap, or timed out is *hard*, never an
expected failure).
"""

from __future__ import annotations

import argparse
from pathlib import Path

TAIL_LINES = 6
ERROR_LINES = 3
SIGNAL_EXIT_BASE = 128  # a shell-style 128 + signal, e.g. 137 for a SIGKILL (a memory-cap kill)


def exe(name: str) -> str:
    """The executable as an absolute path (the children run in a work directory); a bare name
    stays one, for PATH."""
    return str(Path(name).resolve()) if "/" in name else name


def tail(text: str, lines: int = TAIL_LINES) -> str:
    return " | ".join(text.strip().splitlines()[-lines:])


def why(text: str) -> str:
    """A child's failure in one line: its first error lines, else the last lines of its output."""
    errors = [line.strip() for line in text.splitlines() if "error" in line.lower()]
    return " | ".join(errors[:ERROR_LINES]) if errors else tail(text)


def parse_size(text: str) -> int:
    units = {"k": 1 << 10, "m": 1 << 20, "g": 1 << 30}
    scale = units.get(text[-1:].lower(), 1)
    digits = text[:-1] if text[-1:].lower() in units else text
    if not digits.isdigit():
        raise argparse.ArgumentTypeError(f"bad size '{text}'")
    return int(digits) * scale


def is_crash(returncode: int) -> bool:
    """True for an exit that is not the child's own verdict: a signal (negative), or 128 + signal
    (a memory-cap kill seen as an exit status)."""
    return returncode < 0 or returncode >= SIGNAL_EXIT_BASE

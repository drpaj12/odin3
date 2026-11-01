"""uart_text.py REF OUT EXPECT [--cover]: the UART bytes a darksocv testbench run received.

Each trace holds lines "UART <cycle> <hex byte> <char>" (tests/darkriscv/dark_tb.v). The two runs
must have identical UART lines (same cycle, same byte). The decoded text (carriage returns
removed, trailing spaces of a line ignored: the pre-commit hook strips them from expect.txt) must
agree with the expected text: one must be a prefix of the other, and the decoded text must not be
empty. With --cover the decoded text must contain the whole expected text.
Prints the byte count and the decoded character count.
Exit status: 0 agree, 1 differ, 2 usage or read error.
"""
import sys
from pathlib import Path


def uart_lines(path: str) -> list[str]:
    """The UART lines of a trace, in order."""
    return [ln for ln in Path(path).read_text().splitlines() if ln.startswith("UART ")]


def normalise(text: str) -> str:
    """The text without carriage returns and without the trailing spaces of each line."""
    return "\n".join(ln.rstrip(" ") for ln in text.replace("\r", "").split("\n"))


def decode(lines: list[str]) -> str:
    """The text of the received bytes, normalised."""
    return normalise("".join(chr(int(ln.split()[2], 16)) for ln in lines))


def check(ref: list[str], out: list[str], expect: str, cover: bool) -> str | None:
    """None when the runs and the expected text agree, else the reason."""
    if ref != out:
        pairs = enumerate(zip(ref, out, strict=False))
        n = next((i for i, (a, b) in pairs if a != b), min(len(ref), len(out)))
        return f"UART line {n + 1} differs: {ref[n:n + 1]} vs {out[n:n + 1]} ({len(ref)}/{len(out)} lines)"
    text = decode(ref)
    if not text:
        return "no UART byte received"
    if not (expect.startswith(text) or text.startswith(expect)):
        return "decoded text differs from expect.txt"
    if cover and not text.startswith(expect):
        return f"decoded text ({len(text)} chars) does not cover expect.txt ({len(expect)} chars)"
    return None


def main(argv: list[str]) -> int:
    args = [a for a in argv[1:] if a != "--cover"]
    if len(args) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        ref, out = uart_lines(args[0]), uart_lines(args[1])
        expect = normalise(Path(args[2]).read_text())
    except OSError as err:
        print(f"uart_text.py: {err}", file=sys.stderr)
        return 2
    reason = check(ref, out, expect, "--cover" in argv)
    print(f"{len(ref)} UART bytes, {len(decode(ref))} decoded chars")
    if reason is not None:
        print(f"uart_text.py: {reason}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

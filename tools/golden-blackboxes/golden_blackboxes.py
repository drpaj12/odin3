"""golden-blackboxes: collect the distinct ``.model … .blackbox`` stanzas of the goldens.

Usage::

    golden-blackboxes [--golden DIR] [--out DIR] [--write]

Every golden BLIF whose sibling ``.prov`` says ``status=ok`` is read (smallest first) and each
black-box stanza (``.model``, its ``.inputs``/``.outputs`` lines in order, ``.blackbox``, ``.end``)
is keyed by its model name and port lists exactly as written: the oracles list the same ports in
different orders and spellings (``a`` or ``a[0]``), and every variant is a distinct stanza.

Without ``--write`` the distinct stanzas are only summarized.  With ``--write`` each becomes one
fixture ``<out>/<model>.<oracle>.<NN>.blif``: a comment naming the first golden that declares it,
a top model that instantiates it once with every formal connected to a top port, then the stanza.
The 1G tests read every fixture against ``lib/vtr.o3lib`` and round-trip it
(``tests/unit/test_techlib_libs.c``, CTest ``blif_roundtrip_techlib``).

Exit 0 on success, 2 on usage or input error.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Iterator, Sequence
from dataclasses import dataclass
from pathlib import Path

HEADER_LISTS = (".inputs", ".outputs")


class ScanError(Exception):
    """Input error (exit 2)."""


@dataclass
class Stanza:
    """One distinct black-box declaration and where it was first seen."""

    model: str
    inputs: list[str]
    outputs: list[str]
    lines: list[str]  # the header lines as written (comments stripped, continuations joined)
    first: Path
    count: int = 1

    @property
    def key(self) -> tuple[str, ...]:
        return (self.model, *self.lines)

    @property
    def oracle(self) -> str:
        parts = self.first.name.split(".")
        return parts[-2] if len(parts) >= 3 else "blif"


def logical_lines(path: Path) -> Iterator[list[str]]:
    """The statements of a BLIF file as token lists: comments cut, continuations joined."""
    acc: list[str] = []
    with path.open(encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.split("#", 1)[0].rstrip()
            cont = line.endswith("\\")
            acc.extend((line[:-1] if cont else line).split())
            if not cont:
                if acc:
                    yield acc
                acc = []
    if acc:
        yield acc


def stanzas_of(path: Path) -> Iterator[Stanza]:
    """The black-box stanzas of one BLIF file, in file order."""
    model: Stanza | None = None
    blackbox = False
    for toks in logical_lines(path):
        word = toks[0]
        if word == ".model":
            model = Stanza(toks[1] if len(toks) > 1 else "", [], [], [], path)
            blackbox = False
        elif model is not None and word in HEADER_LISTS:
            (model.inputs if word == ".inputs" else model.outputs).extend(toks[1:])
            model.lines.append(" ".join(toks))
        elif model is not None and word == ".blackbox":
            blackbox = True
        elif word == ".end":
            if model is not None and blackbox:
                yield model
            model = None


def golden_blifs(golden: Path) -> list[Path]:
    """Every ``status=ok`` BLIF under golden, smallest first (ties by path)."""
    if not golden.is_dir():
        raise ScanError(f"not a directory: {golden}")
    found = []
    for blif in golden.rglob("*.blif"):
        prov = blif.with_suffix(".prov")
        if prov.is_file() and "status=ok" in prov.read_text(errors="replace").splitlines():
            found.append((blif.stat().st_size, str(blif), blif))
    return [entry[2] for entry in sorted(found)]


def collect(blifs: Sequence[Path]) -> list[Stanza]:
    """Distinct stanzas in first-seen order, with how many files declare each."""
    seen: dict[tuple[str, ...], Stanza] = {}
    for path in blifs:
        for stanza in stanzas_of(path):
            have = seen.get(stanza.key)
            if have is None:
                seen[stanza.key] = stanza
            else:
                have.count += 1
    return list(seen.values())


def fixture_text(stanza: Stanza, golden: Path) -> str:
    """A BLIF that instantiates the stanza's model once from a top model, then declares it."""
    first = stanza.first
    where = first.relative_to(golden) if first.is_relative_to(golden) else first
    formals = [f"{f}=i_{f}" for f in stanza.inputs] + [f"{f}=o_{f}" for f in stanza.outputs]
    out = [
        f"# '{stanza.model}' as {where} declares it ({stanza.count} golden files declare this",
        "# stanza). Written by tools/golden-blackboxes --write; do not edit.",
        ".model top",
    ]
    if stanza.inputs:
        out.append(".inputs " + " ".join(f"i_{f}" for f in stanza.inputs))
    if stanza.outputs:
        out.append(".outputs " + " ".join(f"o_{f}" for f in stanza.outputs))
    out += [f".subckt {stanza.model} " + " ".join(formals), ".end", ""]
    out += [f".model {stanza.model}", *stanza.lines, ".blackbox", ".end"]
    return "\n".join(out) + "\n"


def write_fixtures(stanzas: Sequence[Stanza], golden: Path, out: Path) -> list[Path]:
    """Replaces out's ``*.blif`` with one fixture per stanza; returns the paths written."""
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.blif"):
        old.unlink()
    numbers: dict[tuple[str, str], int] = {}
    written = []
    for stanza in stanzas:
        key = (stanza.model, stanza.oracle)
        numbers[key] = numbers.get(key, 0) + 1
        path = out / f"{stanza.model}.{stanza.oracle}.{numbers[key]:02d}.blif"
        path.write_text(fixture_text(stanza, golden), encoding="utf-8")
        written.append(path)
    return written


def summarize(stanzas: Sequence[Stanza], files: int) -> None:
    print(f"{files} golden BLIFs, {len(stanzas)} distinct black-box stanzas")
    for stanza in stanzas:
        ports = len(stanza.inputs) + len(stanza.outputs)
        print(f"  {stanza.count:5d}  {stanza.oracle:7s} {stanza.model} ({ports} formals)")


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(prog="golden-blackboxes", description=__doc__.split("\n\n")[0])
    repo = Path(__file__).resolve().parents[2]
    ap.add_argument("--golden", type=Path, default=repo.parent / "golden")
    ap.add_argument("--out", type=Path, default=repo / "tests" / "golden" / "techlib")
    ap.add_argument("--write", action="store_true", help="regenerate the fixtures in --out")
    args = ap.parse_args(argv)
    try:
        blifs = golden_blifs(args.golden)
        stanzas = collect(blifs)
        summarize(stanzas, len(blifs))
        if args.write:
            written = write_fixtures(stanzas, args.golden, args.out)
            print(f"wrote {len(written)} fixtures to {args.out}")
    except (ScanError, OSError) as exc:
        print(f"golden-blackboxes: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

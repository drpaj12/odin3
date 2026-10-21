"""Sphinx extension: the pass and command reference, generated at build time (7.3, spec 2.2).

The reference is never copied by hand. ``docs/PASSES.md`` supplies one row per pass (signature,
phase, views, source, tests, notes) and ``odin3 --help`` supplies the registered pass list with
its one-line usage text and the command-line options. ``generate`` joins them and writes the
Markdown that ``docs/user/passes.md`` includes. The module has no Sphinx import (``setup`` takes
the application object untyped) so the tools lint and the unit test can import it.

The odin3 binary is taken from ``passes_ref_odin3`` (conf.py), else ``$ODIN3_CLI``, else
``build/release/odin3`` or ``build/debug/odin3`` under the repository root. No binary is a build
error: the reference is not allowed to fall back to stale text.
"""

from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

_ROW = re.compile(r"^\|\s*`(?P<sig>[^`]*)`(?P<rest>[^|]*)\|(?P<cells>.*)\|\s*$")
_ALIAS = re.compile(r"\(alias\s+`([^`]+)`\)")


@dataclass
class PassRow:
    """One row of docs/PASSES.md."""

    signature: str
    name: str
    aliases: list[str]
    phase: str
    view: str
    source: str
    golden: str
    notes: str


@dataclass
class HelpEntry:
    """One line of the ``passes:`` list of ``odin3 --help``."""

    usage: str
    text: str


def parse_passes_md(text: str) -> list[PassRow]:
    """Rows of the PASSES.md table, in file order.

    The pass cell is a backticked signature (its own ``|`` stay inside the backticks), then
    phase, view, source and golden cells, and the notes cell takes everything left, so a ``|``
    inside the notes survives.
    """
    rows: list[PassRow] = []
    for line in text.splitlines():
        m = _ROW.match(line)
        if m is None:
            continue
        sig = m.group("sig").strip()
        cells = [c.strip() for c in re.split(r"(?<!\\)\|", m.group("cells"), maxsplit=4)]
        cells += [""] * (5 - len(cells))
        rows.append(
            PassRow(
                signature=sig,
                name=sig.split()[0],
                aliases=_ALIAS.findall(m.group("rest")),
                phase=cells[0],
                view=cells[1],
                source=cells[2],
                golden=cells[3],
                notes=cells[4].replace("\\|", "|"),
            )
        )
    return rows


def split_help(text: str) -> tuple[str, str]:
    """``odin3 --help`` split into the text before ``passes:`` and the pass lines after it."""
    head, sep, tail = text.partition("\npasses:\n")
    return (head, tail) if sep else (text, "")


def parse_help(text: str) -> dict[str, HelpEntry]:
    """Pass name -> usage and description from the ``passes:`` list of ``odin3 --help``."""
    out: dict[str, HelpEntry] = {}
    _, tail = split_help(text)
    for line in tail.splitlines():
        if not line.startswith("  ") or not line.strip():
            continue
        body = line.strip()
        usage, colon, desc = body.partition(": ")
        if not colon:
            usage, _, desc = body.partition(" ")
        name = usage.split()[0].rstrip(":")
        out[name] = HelpEntry(usage=usage.rstrip(":"), text=desc.strip())
    return out


def _entry(row: PassRow, helps: dict[str, HelpEntry]) -> str:
    lines = [f"### `{row.name}`", "", "```", row.signature, "```", ""]
    h = helps.get(row.name)
    if h is not None and h.text:
        lines += [h.text, ""]
    if row.aliases:
        lines += ["Alias: " + ", ".join(f"`{a}`" for a in row.aliases) + ".", ""]
    lines += [
        f"- Phase: {row.phase}",
        f"- Views: {row.view}",
        f"- Source: {row.source}",
        f"- Tests: {row.golden}",
    ]
    if row.notes:
        lines += ["", row.notes]
    return "\n".join(lines) + "\n"


def render_markdown(rows: list[PassRow], helps: dict[str, HelpEntry]) -> str:
    """One ``###`` entry per PASSES.md row, joined with the ``--help`` text of the same name."""
    return "\n".join(_entry(r, helps) for r in rows)


def count_entries(markdown: str) -> int:
    """Pass entries in the text ``render_markdown`` returned."""
    return sum(1 for line in markdown.splitlines() if line.startswith("### `"))


def find_odin3(root: Path, configured: str = "") -> Path:
    """The odin3 binary the reference is built from; raises FileNotFoundError."""
    cands = [configured, os.environ.get("ODIN3_CLI", "")]
    cands += [str(root / "build" / p / "odin3") for p in ("release", "debug")]
    for c in cands:
        if c and Path(c).is_file():
            return Path(c)
    raise FileNotFoundError(
        "passes_ref: no odin3 binary (set ODIN3_CLI or passes_ref_odin3, or build "
        "build/release/odin3)"
    )


def generate(root: Path, out_dir: Path, configured: str = "") -> Path:
    """Write ``passes_body.md`` and ``cli_options.md`` into ``out_dir``; return the first."""
    odin3 = find_odin3(root, configured)
    res = subprocess.run([str(odin3), "--help"], capture_output=True, text=True, check=True)
    helps = parse_help(res.stdout)
    rows = parse_passes_md((root / "docs" / "PASSES.md").read_text(encoding="utf-8"))
    known = {r.name for r in rows} | {a for r in rows for a in r.aliases}
    extra = sorted(set(helps) - known)
    body = render_markdown(rows, helps)
    if extra:
        body += "\n### Registered but not in docs/PASSES.md\n\n" + ", ".join(
            f"`{n}`" for n in extra
        ) + "\n"
    out_dir.mkdir(parents=True, exist_ok=True)
    options = split_help(res.stdout)[0].rstrip() + "\n"
    (out_dir / "cli_options.md").write_text("```text\n" + options + "```\n", encoding="utf-8")
    path = out_dir / "passes_body.md"
    path.write_text(body, encoding="utf-8")
    return path


def _on_builder_inited(app: Any) -> None:
    root = Path(app.confdir).resolve().parents[1]
    generate(root, Path(app.confdir) / "_generated", str(app.config.passes_ref_odin3))


def setup(app: Any) -> dict[str, Any]:
    """Sphinx entry point: generate the reference files before the sources are read."""
    app.add_config_value("passes_ref_odin3", "", "env")
    app.connect("builder-inited", _on_builder_inited)
    return {"version": "1", "parallel_read_safe": True}

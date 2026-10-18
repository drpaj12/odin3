"""hard_blocks: hard-block instances declared at the widths they connect (opt-equiv soundness fix).

``expose -evert`` (``opt_equiv.EVERT_BLACKBOXES``) gives each new port the width of the black
box's *declaration* and cuts the instance's connection to it (Yosys 0.55 expose.cc:
``add_new_wire(.., p->width)``, ``sig.extend_u0(w->width)``). A tech library declares
``single_port_ram`` with ``dynports`` and width-1 ``data``/``out``, so only bit 0 of the data was
compared and ``out[N-1:1]`` became undriven, an x, a don't care in the gold: a wrong data or read
bit above 0 passed (3B soundness review C1). Every hard-block instance of the flattened top
therefore gets a type of its own, ``<type>$o3w<k>``, declared with exactly the ports the instance
connects at the widths it connects them (one declaration per distinct (type, widths), the same on
both sides), and the instances that gold and gate share by name must agree on type, port widths
and the parameters both carry (review M1: a ``DATA_WIDTH`` change). The same rule as lower-equiv's
``prepare_boxes`` (fix/3B-heavy-triage 9ac875a4); these are its text helpers, with no Yosys run.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any

BOX_TAG = "$o3w"
USER_CELL = re.compile(r"^\s+cell \\", re.M)
PORT_DECL = re.compile(
    r"^  wire (?P<mid>.*?)\s*(?P<dir>input|output|inout) (?P<num>\d+) (?P<name>\\\S+)$")
END_LINE = "\nend\n"
Widths = tuple[tuple[str, int], ...]


class BoxError(Exception):
    """A hard block the check cannot cut soundly (a tool error, exit 2: never a verdict)."""


@dataclass(frozen=True)
class BoxUse:
    """One hard-block instance: its type, the width of each port it connects, its parameters."""

    kind: str
    widths: Widths
    params: tuple[tuple[str, str], ...]


def has_user_cells(text: str) -> bool:
    """True when an RTLIL text instantiates a module (a cell whose type is not ``$``-internal)."""
    return USER_CELL.search(text) is not None


def rtlil_id(name: str) -> str:
    """An RTLIL identifier from a ``write_json`` name (which drops the ``\\`` of a public one)."""
    return name if name.startswith(("$", "\\")) else "\\" + name


def json_value(value: object) -> str:
    """A ``write_json`` parameter as a comparable text: a bit string becomes its number."""
    text = str(value).rstrip(" ")
    return str(int(text, 2)) if re.fullmatch(r"[01]+", text) else text


def module_span(text: str, top: str) -> tuple[int, int]:
    """(start, end) offsets of the body of module ``top`` in an RTLIL text (after its header
    line, up to its ``end``); (-1, -1) when it is absent."""
    head = f"module {top}\n"
    if text.startswith(head):
        start = len(head)
    else:
        found = text.find("\n" + head)
        if found < 0:
            return -1, -1
        start = found + 1 + len(head)
    return start, text.index(END_LINE, start - 1) + 1


def blackbox_modules(text: str) -> dict[str, str]:
    """name -> RTLIL text (attributes included) of every ``blackbox`` module of an RTLIL text: the
    hard blocks (``single_port_ram``, ``dual_port_ram``, ``multiply``) a tech library declares."""
    found: dict[str, str] = {}
    lines = text.splitlines(keepends=True)
    attrs: list[str] = []
    number = 0
    while number < len(lines):
        line = lines[number]
        if line.startswith("attribute "):
            attrs.append(line)
        elif line.startswith("module "):
            end = number
            while end < len(lines) and lines[end].rstrip("\n") != "end":
                end += 1
            if any(a.split()[:3] == ["attribute", "\\blackbox", "1"] for a in attrs):
                found[line.split()[1]] = "".join([*attrs, *lines[number:end + 1]])
            attrs = []
            number = end
        else:
            attrs = []
        number += 1
    return found


def box_uses(design: dict[str, Any], top: str, boxes: set[str]) -> dict[str, BoxUse]:
    """instance name -> ``BoxUse`` of the hard blocks of the flattened ``top`` (a ``write_json``
    design). ``BoxError`` for a hard block left in another module (``keep_hierarchy``) and for a
    user cell whose type has no module: ``expose -evert`` would drop it and leave its outputs
    undriven (an x, a don't care), so it would never be compared."""
    modules = {rtlil_id(name) for name in design["modules"]}
    top_name = top[1:] if top.startswith("\\") else top
    for name, module in design["modules"].items():
        if name != top_name and any(rtlil_id(c["type"]) in boxes
                                     for c in module.get("cells", {}).values()):
            raise BoxError(f"module {rtlil_id(name)} holds a hard block after flatten")
    uses: dict[str, BoxUse] = {}
    for name, cell in design["modules"][top_name]["cells"].items():
        kind = rtlil_id(cell["type"])
        if kind.startswith("$"):
            continue
        if kind not in modules:
            raise BoxError(f"cell {rtlil_id(name)} has the undeclared type {kind}")
        if kind in boxes:
            uses[rtlil_id(name)] = BoxUse(
                kind, tuple(sorted((rtlil_id(port), len(bits))
                                   for port, bits in cell["connections"].items())),
                tuple(sorted((rtlil_id(key), json_value(value))
                             for key, value in cell.get("parameters", {}).items())))
    return uses


def box_mismatch(gold: dict[str, BoxUse], gate: dict[str, BoxUse]) -> str:
    """The first hard block that gold and gate both hold under one name but with another type,
    other port widths or another value of a parameter both carry; '' when there is none."""
    for name in sorted(gold.keys() & gate.keys()):
        mine, theirs = gold[name], gate[name]
        params = dict(theirs.params)
        if mine.kind != theirs.kind or mine.widths != theirs.widths or any(
                key in params and params[key] != value for key, value in mine.params):
            return f"hard block {name} differs: gold {describe(mine)}; gate {describe(theirs)}"
    return ""


def describe(use: BoxUse) -> str:
    """``single_port_ram (addr 4, data 4, ...; ADDR_WIDTH=4, ...)``."""
    ports = ", ".join(f"{port[1:]} {width}" for port, width in use.widths)
    params = ", ".join(f"{key[1:]}={value}" for key, value in use.params)
    return f"{use.kind[1:]} ({ports}; {params})"


def box_declaration(module: str, name: str, widths: dict[str, int]) -> str:
    """The black-box ``module`` text renamed ``name`` with exactly the ports in ``widths``, at
    those widths (``dynports`` and parameters dropped). ``BoxError`` when the instance connects a
    port the declaration lacks."""
    declared = {m.group("name") for m in map(PORT_DECL.match, module.splitlines()) if m}
    if not set(widths) <= declared:
        raise BoxError(f"a hard block connects {sorted(set(widths) - declared)}, which its "
                       "declaration lacks")
    lines, number = ["attribute \\blackbox 1", f"module {name}"], 0
    for line in module.splitlines():
        found = PORT_DECL.match(line)
        if found is None or found.group("name") not in widths:
            continue
        number += 1
        port = found.group("name")
        size = f"width {widths[port]} " if widths[port] > 1 else ""
        lines.append(f"  wire {size}{found.group('dir')} {number} {port}")
    return "\n".join([*lines, "end"]) + "\n"


def retype_boxes(text: str, top: str, uses: dict[str, BoxUse],
                 names: dict[tuple[str, Widths], str]) -> str:
    """``text`` (a flattened RTLIL design) with each hard-block instance of ``top`` given its
    specialised type from ``names`` ((type, widths) -> new type)."""
    start, end = module_span(text, top)
    if start < 0:
        raise BoxError(f"no module {top} in the flattened design")
    body = []
    for line in text[start:end].splitlines(keepends=True):
        words = line.split()
        if len(words) == 3 and words[0] == "cell" and words[2] in uses:
            use = uses[words[2]]
            line = line.replace(f"cell {use.kind} ", f"cell {names[(use.kind, use.widths)]} ", 1)
        body.append(line)
    return text[:start] + "".join(body) + text[end:]


def mark_top(text: str, top: str) -> str:
    """``text`` with ``attribute \\top 1`` before module ``top`` (and on no other module)."""
    out = re.sub(r"^attribute \\top 1\s*\n", "", text, flags=re.M)
    return re.sub(rf"^module {re.escape(top)}\s*$", lambda _: f"attribute \\top 1\nmodule {top}",
                  out, count=1, flags=re.M)


def declarations(modules: dict[str, str], uses: list[dict[str, BoxUse]]
                 ) -> tuple[dict[tuple[str, Widths], str], str]:
    """((type, widths) -> new type, the RTLIL declarations of the new types) for the hard-block
    instances of both sides: one declaration per distinct (type, widths), shared by the sides."""
    names: dict[tuple[str, Widths], str] = {}
    text = ""
    for use in sorted({(u.kind, u.widths) for side in uses for u in side.values()}):
        names[use] = f"{use[0]}{BOX_TAG}{len(names)}"
        text += box_declaration(modules[use[0]], names[use], dict(use[1]))
    return names, text

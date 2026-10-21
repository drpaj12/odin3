"""Load an Odin III Yosys-schema JSON netlist as a graph, without Odin III (spec 6.3, E-4, E-5).

Pure Python plus the standard library. It reads the file `write_json` (or `write_json --odin3`)
writes and does not import `odin3.py` or need `libodin3`. networkx is imported only by `load_json`,
torch and torch_geometric only by `to_pyg`.

Graph (E-4). One node per cell and one node per module port (the port is one node, not one per
bit). Constant bits ("0", "1", "x", "z") are not nodes. One edge per driver-to-sink pin pair on a
net whose driver is a node, so a 4-bit bus between two nodes gives four edges (a `MultiDiGraph`).
A driver is an output or inout pin of a cell, or an input or inout module port; a sink is an input
or inout pin of a cell, or an output or inout module port.

  node attributes: kind ("cell" or "port"), type, parameters, attributes, and when the file has
                   them odin3_level (cells only) and odin3_prov.
  edge attributes: net (the net name), net_bit (the bit of that name), driver_port, sink_port.

Version policy (E-8, J-9): a file whose `odin3_version` is present and is not the integer 1 is
refused. A malformed file raises GraphError. A cell key equal to a port name raises GraphError too
(one node id for two nodes would merge them).

Run: python3 plugins/python/odin3_graph.py FILE.json [MODULE]   (prints the node and edge counts)
"""

from __future__ import annotations

import json
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any, NamedTuple

SUPPORTED_VERSION = 1
_PORT_TYPE = {"input": "$port_in", "output": "$port_out", "inout": "$port_inout"}
_DRIVES = ("output", "inout")
_SINKS = ("input", "inout")


class GraphError(ValueError):
    """The file is not a netlist this loader can read."""


class Node(NamedTuple):
    """One node: its id (the cell key or the port name) and its attributes."""

    ident: str
    attrs: dict[str, Any]


class Edge(NamedTuple):
    """One driver-to-sink pin pair."""

    source: str
    target: str
    attrs: dict[str, Any]


class _Pin(NamedTuple):
    node: str
    port: str


def _check_version(doc: dict[str, Any]) -> None:
    if "odin3_version" not in doc:
        return
    version = doc["odin3_version"]
    if type(version) is not int:  # a bool is an int in Python: refuse it too
        raise GraphError(f"odin3_version {version!r} is not an integer")
    if version > SUPPORTED_VERSION:
        raise GraphError(
            f"odin3_version {version}: written by a newer Odin III (this loader knows "
            f"{SUPPORTED_VERSION})"
        )
    if version != SUPPORTED_VERSION:
        raise GraphError(f"odin3_version {version} is not a version (this loader knows 1)")


def _pick_module(doc: dict[str, Any], module: str | None) -> tuple[str, dict[str, Any]]:
    modules = doc.get("modules")
    if not isinstance(modules, dict) or not modules:
        raise GraphError("the file has no modules")
    if module is not None:
        if module not in modules:
            raise GraphError(f"no module '{module}' (modules: {', '.join(modules)})")
        return module, modules[module]
    real = [name for name, body in modules.items() if not _blackbox(body)]
    if len(real) != 1:
        raise GraphError(f"pass module=...: the file has {len(real)} modules ({', '.join(real)})")
    return real[0], modules[real[0]]


def _blackbox(body: dict[str, Any]) -> bool:
    flag = body.get("attributes", {}).get("blackbox")
    return flag not in (None, 0, "0", "00000000000000000000000000000000")


def _bit_labels(bits: Any) -> Iterator[tuple[int, int | None]]:
    """(bus index, net label) for every bit; the label is None for a constant or "x"."""
    for index, bit in enumerate(bits):
        yield index, bit if isinstance(bit, int) else None


def _net_names(body: dict[str, Any]) -> dict[int, tuple[str, int]]:
    """For each net label, the first netname that holds it (a visible name first) and its bit."""
    names: dict[int, tuple[str, int]] = {}
    hidden: dict[int, tuple[str, int]] = {}
    for name, entry in body.get("netnames", {}).items():
        target = hidden if entry.get("hide_name") else names
        for index, label in _bit_labels(entry.get("bits", [])):
            if label is not None:
                target.setdefault(label, (name, index))
    for label, found in hidden.items():
        names.setdefault(label, found)
    return names


def _port_node(name: str, port: dict[str, Any], body: dict[str, Any]) -> Node:
    attrs: dict[str, Any] = {
        "kind": "port",
        "type": _PORT_TYPE.get(port.get("direction", ""), "$port_inout"),
        "parameters": {"WIDTH": len(port.get("bits", []))},
        "attributes": {},
    }
    prov = body.get("netnames", {}).get(name, {}).get("odin3_prov")
    if prov is not None:
        attrs["odin3_prov"] = prov
    return Node(name, attrs)


def _cell_node(key: str, cell: dict[str, Any]) -> Node:
    attrs: dict[str, Any] = {
        "kind": "cell",
        "type": cell.get("type", ""),
        "parameters": dict(cell.get("parameters", {})),
        "attributes": dict(cell.get("attributes", {})),
    }
    for extra in ("odin3_level", "odin3_prov"):
        if extra in cell:
            attrs[extra] = cell[extra]
    return Node(key, attrs)


def _pins(body: dict[str, Any]) -> dict[int, tuple[list[_Pin], list[_Pin]]]:
    """For each net label, its (drivers, sinks)."""
    pins: dict[int, tuple[list[_Pin], list[_Pin]]] = {}

    def add(label: int, pin: _Pin, direction: str) -> None:
        drivers, sinks = pins.setdefault(label, ([], []))
        if direction in _DRIVES:
            drivers.append(pin)
        if direction in _SINKS:
            sinks.append(pin)

    for name, port in body.get("ports", {}).items():
        # a module input drives the module's nets, a module output is driven by them
        flipped = {"input": "output", "output": "input"}.get(port.get("direction", ""), "inout")
        for _, label in _bit_labels(port.get("bits", [])):
            if label is not None:
                add(label, _Pin(name, name), flipped)
    for key, cell in body.get("cells", {}).items():
        directions = cell.get("port_directions", {})
        for port_name, bits in cell.get("connections", {}).items():
            for _, label in _bit_labels(bits):
                if label is not None:
                    add(label, _Pin(key, port_name), directions.get(port_name, "input"))
    return pins


def parse_graph(path: str | Path, module: str | None = None) -> tuple[list[Node], list[Edge]]:
    """The nodes and edges of one module of the JSON file at path (E-4), in file order."""
    try:
        with open(path, encoding="utf-8") as handle:
            doc = json.load(handle)
    except ValueError as err:  # JSONDecodeError and UnicodeDecodeError
        raise GraphError(f"not JSON: {err}") from err
    if not isinstance(doc, dict):
        raise GraphError("the root is not an object")
    _check_version(doc)
    try:
        return _graph_of(doc, module)
    except (AttributeError, TypeError, KeyError, IndexError) as err:
        raise GraphError(f"not a netlist: {err!r}") from err


def _graph_of(doc: dict[str, Any], module: str | None) -> tuple[list[Node], list[Edge]]:
    _, body = _pick_module(doc, module)
    nodes = [_port_node(name, port, body) for name, port in body.get("ports", {}).items()]
    nodes += [_cell_node(key, cell) for key, cell in body.get("cells", {}).items()]
    seen: set[str] = set()
    for node in nodes:
        if node.ident in seen:
            raise GraphError(f"'{node.ident}' names both a port and a cell")
        seen.add(node.ident)
    names = _net_names(body)
    edges: list[Edge] = []
    pins = _pins(body)
    for label in sorted(pins):
        drivers, sinks = pins[label]
        net, net_bit = names.get(label, (f"${label}", 0))
        for driver in drivers:
            for sink in sinks:
                if driver == sink:
                    continue
                edges.append(
                    Edge(
                        driver.node,
                        sink.node,
                        {
                            "net": net,
                            "net_bit": net_bit,
                            "driver_port": driver.port,
                            "sink_port": sink.port,
                        },
                    )
                )
    return nodes, edges


def load_json(path: str | Path, module: str | None = None) -> Any:
    """The module as a networkx.MultiDiGraph (a file with one module needs no `module`)."""
    import networkx

    nodes, edges = parse_graph(path, module)
    graph = networkx.MultiDiGraph()
    for node in nodes:
        graph.add_node(node.ident, **node.attrs)
    for edge in edges:
        graph.add_edge(edge.source, edge.target, **edge.attrs)
    return graph


def to_pyg(graph: Any) -> Any:
    """A torch_geometric.data.Data of a graph from load_json.

    x has one row per node (in graph order): the index of its type in `type_names` and its
    odin3_level (-1 when it has none). edge_index is the edge list in graph order. Names are kept
    as `node_ids` and `type_names`.
    """
    import torch
    from torch_geometric.data import Data

    ids = list(graph.nodes)
    index = {ident: i for i, ident in enumerate(ids)}
    type_names = sorted({graph.nodes[ident]["type"] for ident in ids})
    rows = [
        [
            float(type_names.index(graph.nodes[ident]["type"])),
            float(graph.nodes[ident].get("odin3_level", -1)),
        ]
        for ident in ids
    ]
    edge_pairs = [(index[src], index[dst]) for src, dst in graph.edges()]
    edge_index = torch.tensor(edge_pairs, dtype=torch.long).t().contiguous()
    if not edge_pairs:
        edge_index = torch.empty((2, 0), dtype=torch.long)
    data = Data(x=torch.tensor(rows, dtype=torch.float), edge_index=edge_index)
    data.node_ids = ids
    data.type_names = type_names
    return data


def main(argv: list[str]) -> int:
    if not 1 <= len(argv) <= 2:
        print("usage: odin3_graph.py FILE.json [MODULE]", file=sys.stderr)
        return 2
    try:
        nodes, edges = parse_graph(argv[0], argv[1] if len(argv) > 1 else None)
    except (GraphError, OSError) as err:
        print(f"odin3_graph: {err}", file=sys.stderr)
        return 1
    print(f"nodes {len(nodes)} edges {len(edges)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

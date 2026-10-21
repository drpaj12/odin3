"""Phase 6, 6.3: the graph export (`write_json --odin3`) and plugins/python/odin3_graph.py.

Three CTests run these classes:
  graph_loader_isolated   IsolatedLoaderTest: a temp copy of the loader in a clean interpreter
                          (`python3 -I`, empty environment, no libodin3) reads a hand-written file,
                          and refuses a newer `odin3_version` (E-5, E-8).
  graph_export_counts     CountsTest: for each micro the loader's node and edge counts equal the
                          IR's (E-6), and levels order the edges (E-7).
  graph_loader_packages   PackagesTest: load_json and to_pyg, when networkx and torch_geometric
                          import (a named skip otherwise).
  graph_export_roundtrip  RoundTripTest: write A, read, write B, read, write C with the CLI; A and
                          B are equal without the `odin3_prov` objects and B equals C (E-3).

The reference counts share no code with the loader. The loader counts from the JSON text. The
reference counts nodes from the `dump_locs` pass (one line per node) and edges from the C ABI
(`plugins/python/odin3.py`: pins and nets). Every skip names its reason and CTest reports it as
Skipped (SKIP_REGULAR_EXPRESSION), never as Passed.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING, Any

REPO_ROOT = Path(__file__).resolve().parents[2]
LOADER = REPO_ROOT / "plugins" / "python" / "odin3_graph.py"
VTR_LIB = REPO_ROOT / "lib" / "vtr.o3lib"
BLIF_DIR = REPO_ROOT / "tests" / "golden" / "blif"
# The micros of the default set: committed BLIF designs, bit level, that read_blif accepts.
MICROS = (
    "ff.parmys",
    "ff.odin",
    "dffsre.parmys",
    "adder_hard_block.parmys",
    "ansiportlist_2.parmys",
    "pow.parmys",
    "hand_body",
    "hand_cname",
    "hand_ports",
    "hand_wide",
    "hand_writers",
)
# levelize refuses a word-level cell (V-1); pow.parmys has a $pow.
# hand_ports has ports and black boxes but no cell: E-7 has nothing to compare there.
CELLLESS = ("hand_ports",)
LEVELIZABLE = tuple(micro for micro in MICROS if micro != "pow.parmys")
CLI = os.environ.get("ODIN3_CLI")
HAVE_LIB = bool(os.environ.get("ODIN3_LIB"))
REQUIRE_CFFI = os.environ.get("ODIN3_REQUIRE_CFFI") == "1"

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False
try:
    import networkx

    HAVE_NETWORKX = True
except ImportError:
    HAVE_NETWORKX = False
try:
    import torch_geometric  # noqa: F401

    HAVE_PYG = True
except ImportError:
    HAVE_PYG = False

sys.path.insert(0, str(REPO_ROOT / "plugins" / "python"))
import odin3_graph  # noqa: E402  (the loader: used only for the file's own output, never to count)

if TYPE_CHECKING or HAVE_CFFI:
    from odin3 import Design, Module, Odin3

SIMPLE = {
    "creator": "t",
    "odin3_version": 1,
    "modules": {
        "m": {
            "attributes": {},
            "ports": {
                "a": {"direction": "input", "bits": [2, 3]},
                "y": {"direction": "output", "bits": [4, "0"]},
            },
            "cells": {
                "g": {
                    "hide_name": 0,
                    "type": "$_AND_",
                    "parameters": {},
                    "attributes": {},
                    "port_directions": {"A": "input", "B": "input", "Y": "output"},
                    "connections": {"A": [2], "B": [3], "Y": [4]},
                    "odin3_level": 1,
                }
            },
            "netnames": {
                "a": {"hide_name": 0, "bits": [2, 3], "attributes": {}},
                "y": {"hide_name": 0, "bits": [4, "0"], "attributes": {}},
            },
        }
    },
}
SIMPLE_NODES = 3
SIMPLE_EDGES = 3  # a[0] -> g.A, a[1] -> g.B, g.Y -> y

ISOLATED_SCRIPT = """
import importlib.util, json, sys
spec = importlib.util.spec_from_file_location("odin3_graph", sys.argv[1])
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
assert "odin3" not in sys.modules and "cffi" not in sys.modules, sorted(sys.modules)
nodes, edges = mod.parse_graph(sys.argv[2])
print(len(nodes), len(edges))
doc = json.load(open(sys.argv[2]))
doc["odin3_version"] = 2
json.dump(doc, open(sys.argv[3], "w"))
try:
    mod.parse_graph(sys.argv[3])
except mod.GraphError as err:
    print("refused:", err)
else:
    sys.exit("odin3_version 2 was accepted")
"""


class IsolatedLoaderTest(unittest.TestCase):
    """E-5: the loader runs from a temp copy in a clean interpreter, with no Odin III around."""

    def test_isolated_copy_counts_and_refuses_new_version(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "odin3_graph.py"
            copy.write_bytes(LOADER.read_bytes())
            good = Path(tmp) / "good.json"
            good.write_text(json.dumps(SIMPLE), encoding="utf-8")
            newer = Path(tmp) / "newer.json"
            done = subprocess.run(
                [sys.executable, "-I", "-c", ISOLATED_SCRIPT, str(copy), str(good), str(newer)],
                capture_output=True,
                text=True,
                cwd=tmp,
                env={},
                check=False,
            )
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn(f"{SIMPLE_NODES} {SIMPLE_EDGES}", done.stdout)
            self.assertIn("refused: odin3_version 2", done.stdout)

    def test_hand_file_shape(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "s.json"
            path.write_text(json.dumps(SIMPLE), encoding="utf-8")
            nodes, edges = odin3_graph.parse_graph(path)
        kinds = {node.ident: node.attrs["kind"] for node in nodes}
        self.assertEqual(kinds, {"a": "port", "y": "port", "g": "cell"})
        by_ident = {node.ident: node.attrs for node in nodes}
        self.assertEqual(by_ident["g"]["odin3_level"], 1)
        self.assertEqual(by_ident["a"]["parameters"], {"WIDTH": 2})
        pairs = sorted((e.source, e.target, e.attrs["sink_port"]) for e in edges)
        self.assertEqual(pairs, [("a", "g", "A"), ("a", "g", "B"), ("g", "y", "y")])

    def test_several_modules_need_a_name(self) -> None:
        doc = json.loads(json.dumps(SIMPLE))
        doc["modules"]["n"] = doc["modules"]["m"]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "two.json"
            path.write_text(json.dumps(doc), encoding="utf-8")
            with self.assertRaises(odin3_graph.GraphError):
                odin3_graph.parse_graph(path)
            nodes, _ = odin3_graph.parse_graph(path, "n")
        self.assertEqual(len(nodes), SIMPLE_NODES)

    def write(self, tmp: str, doc: Any, raw: str | None = None) -> Path:
        path = Path(tmp) / "x.json"
        path.write_text(raw if raw is not None else json.dumps(doc), encoding="utf-8")
        return path

    def test_version_must_be_the_integer_one(self) -> None:
        bad_versions: list[Any] = [True, False, None, "1", 1.0, [1], {}, 0, -1, 2]
        for bad in bad_versions:
            doc = json.loads(json.dumps(SIMPLE))
            doc["odin3_version"] = bad
            with (
                tempfile.TemporaryDirectory() as tmp,
                self.subTest(version=bad),
                self.assertRaises(odin3_graph.GraphError),
            ):
                odin3_graph.parse_graph(self.write(tmp, doc))
        doc = json.loads(json.dumps(SIMPLE))
        del doc["odin3_version"]  # an absent version is a plain Yosys file
        with tempfile.TemporaryDirectory() as tmp:
            nodes, _ = odin3_graph.parse_graph(self.write(tmp, doc))
        self.assertEqual(len(nodes), SIMPLE_NODES)

    def test_cell_key_equal_to_port_name_is_refused(self) -> None:
        doc = json.loads(json.dumps(SIMPLE))
        doc["modules"]["m"]["cells"]["a"] = doc["modules"]["m"]["cells"]["g"]
        with (
            tempfile.TemporaryDirectory() as tmp,
            self.assertRaises(odin3_graph.GraphError) as caught,
        ):
            odin3_graph.parse_graph(self.write(tmp, doc))
        self.assertIn("'a'", str(caught.exception))

    def test_malformed_files_raise_graph_error(self) -> None:
        def mutated(change: Any) -> dict[str, Any]:
            doc = json.loads(json.dumps(SIMPLE))
            change(doc["modules"]["m"])
            return doc  # type: ignore[no-any-return]

        cases: list[Any] = [
            mutated(lambda m: m.update(cells=[1])),
            mutated(lambda m: m.update(cells={"g": 3})),
            mutated(lambda m: m["cells"]["g"].update(connections=[1])),
            mutated(lambda m: m["cells"]["g"].update(connections={"A": 5})),
            mutated(lambda m: m.update(ports={"a": "x"})),
            mutated(lambda m: m.update(netnames={"n": []})),
            mutated(lambda m: m.update(attributes=7)),
            {"modules": {"m": 3}},
            {"modules": {"m": {"ports": {"a": {"bits": 4}}}}},
        ]
        for case in cases:
            with tempfile.TemporaryDirectory() as tmp, self.subTest(case=case):
                path = self.write(tmp, case)
                with self.assertRaises(odin3_graph.GraphError):
                    odin3_graph.parse_graph(path)
                self.assertEqual(odin3_graph.main([str(path)]), 1)
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write(tmp, None, raw="{not json")
            with self.assertRaises(odin3_graph.GraphError):
                odin3_graph.parse_graph(path)
            self.assertEqual(odin3_graph.main([str(path)]), 1)


def cli_env() -> dict[str, str]:
    """Environment for a child odin3 CLI: no LD_PRELOAD (CMake preloads the ASan runtime for the
    cffi library in this process; the CLI links its own runtime and aborts on a second one)."""
    return {k: v for k, v in os.environ.items() if k not in ("LD_PRELOAD", "LD_AUDIT")}


def run_cli(script: str) -> str:
    """Run an odin3 script (-p) and return its stdout."""
    assert CLI is not None
    done = subprocess.run(
        [CLI, "-p", script], capture_output=True,
        text=True,
        check=False,
        cwd=REPO_ROOT,
        env=cli_env(),
    )
    if done.returncode != 0:
        raise AssertionError(f"odin3 -p '{script}' failed:\n{done.stdout}\n{done.stderr}")
    return done.stdout


def read_micro(micro: str) -> str:
    return f"read_techlib {VTR_LIB}; read_blif {BLIF_DIR / (micro + '.blif')}"


def is_constant_cell(node: Any) -> bool:
    """A cell the JSON writer leaves out because its nets are constants (E-4)."""
    name = node.type_name
    if name.startswith("$_CONST"):
        return True
    return name == "$sop" and all(pin.direction() == "out" for pin in node.pins())


def reference_edges(module: Module) -> int:
    """Edges of the E-4 definition from the IR: for each net not driven by a constant cell, one
    edge per (driver pin, sink pin) pair."""
    total = 0
    for net in module.nets():
        pins = net.pins()
        drivers = [pin for pin in pins if pin.direction() in ("out", "inout")]
        if any(is_constant_cell(pin.node()) for pin in drivers):
            continue
        sinks = [pin for pin in pins if pin.direction() in ("in", "inout")]
        total += sum(1 for d in drivers for s in sinks if d != s)
    return total


def dump_node_counts(text: str) -> dict[str, int]:
    """The `module|node|name|SRC` lines of dump_locs, counted per module."""
    counts: dict[str, int] = {}
    for line in text.splitlines():
        fields = re.split(r"(?<!\\)\|", line, maxsplit=3)
        if len(fields) >= 3 and fields[1] == "node":
            counts[fields[0]] = counts.get(fields[0], 0) + 1
    return counts


def needs_tools(test: unittest.TestCase) -> None:
    """Named skips: nothing here passes silently."""
    if not CLI:
        test.skipTest("ODIN3_CLI not set (run CTest graph_export_counts)")
    if not HAVE_LIB:
        test.skipTest("ODIN3_LIB not set (run CTest graph_export_counts)")
    if not HAVE_CFFI:
        if REQUIRE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        test.skipTest("cffi not installed")


class CountsTest(unittest.TestCase):
    odin3: Odin3
    tmp: tempfile.TemporaryDirectory[str]

    @classmethod
    def setUpClass(cls) -> None:
        if HAVE_CFFI and HAVE_LIB:
            cls.odin3 = Odin3()
            cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_ERROR)
        cls.tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def export(self, micro: str, levelize: bool) -> tuple[Path, dict[str, int]]:
        out = Path(self.tmp.name) / f"{micro}{'.lv' if levelize else ''}.json"
        steps = [read_micro(micro)] + (["levelize"] if levelize else [])
        steps += [f"write_json {out} --odin3", "dump_locs"]
        return out, dump_node_counts(run_cli("; ".join(steps)))

    def design(self, micro: str) -> Design:
        return Design.read_blif(BLIF_DIR / f"{micro}.blif", techlibs=[VTR_LIB], odin3=self.odin3)

    def test_counts_equal_the_ir(self) -> None:
        needs_tools(self)
        for micro in MICROS:
            with self.subTest(micro=micro):
                path, dumped = self.export(micro, levelize=False)
                with self.design(micro) as design:
                    for module in design.modules():
                        consts = sum(1 for node in module.nodes() if is_constant_cell(node))
                        nodes = dumped[module.name] - consts
                        self.assertEqual(nodes, len(list(module.nodes())) - consts)
                        got_nodes, got_edges = odin3_graph.parse_graph(path, module.name)
                        self.assertEqual(len(got_nodes), nodes, module.name)
                        self.assertEqual(len(got_edges), reference_edges(module), module.name)

    def test_levels_order_the_edges(self) -> None:
        """E-7: an edge into a combinational, not cut cell comes from a strictly lower level.
        The registry flags are not in the C ABI, so a sequential cell (and any cell that starts
        a path) is read from the levelize result: level 0 with inputs. Edges into those, into cut
        cells and into port nodes are exempt."""
        needs_tools(self)
        for micro in LEVELIZABLE:
            with self.subTest(micro=micro):
                path, _ = self.export(micro, levelize=True)
                with self.design(micro) as design:
                    names = [module.name for module in design.modules()]
                compared = 0
                deepest = 0
                for name in names:
                    count, level = self.check_levels(path, name)
                    compared += count
                    deepest = max(deepest, level)
                if micro in CELLLESS:
                    self.assertEqual((deepest, compared), (0, 0), micro)
                    continue
                self.assertGreater(deepest, 0, f"{micro}: no cell above level 0 (vacuous)")
                self.assertGreater(compared, 0, f"{micro}: no edge was compared (vacuous)")

    def check_levels(self, path: Path, module: str) -> tuple[int, int]:
        """Checks the module; returns (edges E-7 compared, the highest cell level)."""
        nodes, edges = odin3_graph.parse_graph(path, module)
        attrs = {node.ident: node.attrs for node in nodes}
        cells = [a for a in attrs.values() if a["kind"] == "cell"]
        self.assertTrue(all("odin3_level" in a for a in cells), "a cell has no odin3_level")
        self.assertTrue(all("odin3_level" not in a for a in attrs.values() if a["kind"] == "port"))
        self.assertTrue(all("odin3_level" not in a["attributes"] for a in cells))
        compared = 0
        for edge in edges:
            target = attrs[edge.target]
            if target["kind"] == "port" or "odin3_level_cut" in target["attributes"]:
                continue
            if target["odin3_level"] == 0:
                continue  # a register, latch or memory: it ends and starts a path
            source = attrs[edge.source]
            level = source.get("odin3_level", 0)  # a port node counts as level 0
            self.assertLess(level, target["odin3_level"], f"{edge.source} -> {edge.target}")
            compared += 1
        return compared, max((a["odin3_level"] for a in cells), default=0)


class PackagesTest(unittest.TestCase):
    """The networkx and Torch paths, on the hand-written file; named skips without the packages."""

    def test_networkx_graph_has_the_same_counts(self) -> None:
        if not HAVE_NETWORKX:
            self.skipTest("networkx not installed")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "s.json"
            path.write_text(json.dumps(SIMPLE), encoding="utf-8")
            graph = odin3_graph.load_json(path)
            self.assertIsInstance(graph, networkx.MultiDiGraph)
            self.assertEqual(graph.number_of_nodes(), SIMPLE_NODES)
            self.assertEqual(graph.number_of_edges(), SIMPLE_EDGES)
            self.assertEqual(graph.nodes["g"]["odin3_level"], 1)

    def test_pyg_data_has_the_same_counts(self) -> None:
        if not (HAVE_NETWORKX and HAVE_PYG):
            self.skipTest("networkx or torch_geometric not installed")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "s.json"
            path.write_text(json.dumps(SIMPLE), encoding="utf-8")
            data = odin3_graph.to_pyg(odin3_graph.load_json(path))
        self.assertEqual(data.num_nodes, SIMPLE_NODES)
        self.assertEqual(data.num_edges, SIMPLE_EDGES)


_PROV = re.compile(r',\n\s*"odin3_prov": \{[^}]*\}')
_LEVEL = re.compile(r',\n\s*"odin3_level": -?[0-9]+')
_VERSION = '  "odin3_version": 1,\n'
# Micros whose own default `write_json` output `read_json` already refuses (check rule 4: a net
# with sinks and no driver; hand_ports has no such cell either). That is independent of --odin3.
UNREADABLE = ("dffsre.parmys", "ansiportlist_2.parmys", "hand_ports")
READABLE = tuple(micro for micro in MICROS if micro not in UNREADABLE)


def strip_odin3(text: str) -> str:
    """The text without the Odin III keys: what a default write of the same design holds, apart
    from `odin3_level`, which is also an attribute there (not levelized micros have none)."""
    return _LEVEL.sub("", _PROV.sub("", text)).replace(_VERSION, "")


def cell_levels(path: Path) -> dict[tuple[str, str], int]:
    doc = json.loads(path.read_text(encoding="utf-8"))
    return {
        (module, key): cell["odin3_level"]
        for module, body in doc["modules"].items()
        for key, cell in body.get("cells", {}).items()
        if "odin3_level" in cell
    }


class RoundTripTest(unittest.TestCase):
    """E-3: write A, read, write B, read, write C, with the CLI, for each micro.

    Spec (i) says A and B are equal once `odin3_prov` is dropped. That cannot hold for any file
    whose bit numbers have gaps (the reader numbers nets by ascending bit and the constants last),
    with or without --odin3: the same A and B differ for a default write of ff.parmys. So (i) is
    tested as what --odin3 adds: A without the Odin III keys is the default write of the design,
    and B without them is the default write after one read (a). (ii) B equals C is exact (b).
    Levels survive the trip, by cell (c).
    """

    def setUp(self) -> None:
        if not CLI:
            self.skipTest("ODIN3_CLI not set (run CTest graph_export_roundtrip)")

    def test_keys_only_add(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for micro in READABLE:
                with self.subTest(micro=micro):
                    paths = {n: Path(tmp) / f"{micro}.{n}.json" for n in ("a", "b", "c", "d", "e")}
                    run_cli(f"{read_micro(micro)}; write_json {paths['a']} --odin3")
                    run_cli(f"{read_micro(micro)}; write_json {paths['d']}")
                    run_cli(f"read_json {paths['a']}; write_json {paths['b']} --odin3")
                    run_cli(f"read_json {paths['d']}; write_json {paths['e']}")
                    run_cli(f"read_json {paths['b']}; write_json {paths['c']} --odin3")
                    text = {n: path.read_text() for n, path in paths.items()}
                    self.assertIn('"odin3_version": 1', text["a"])
                    self.assertEqual(strip_odin3(text["a"]), text["d"])  # (i), A
                    self.assertEqual(strip_odin3(text["b"]), text["e"])  # (i), B
                    self.assertEqual(text["b"], text["c"])  # (ii)

    def test_levels_survive(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for micro in (m for m in READABLE if m in LEVELIZABLE):
                with self.subTest(micro=micro):
                    a, b = (Path(tmp) / f"{micro}.{n}.json" for n in "ab")
                    run_cli(f"{read_micro(micro)}; levelize; write_json {a} --odin3")
                    run_cli(f"read_json {a}; write_json {b} --odin3")
                    levels = cell_levels(a)
                    self.assertTrue(levels)
                    self.assertEqual(levels, cell_levels(b))

    def test_unreadable_micros_stay_unreadable_by_default(self) -> None:
        """If the reader starts to accept one of them, move it to READABLE."""
        assert CLI is not None
        with tempfile.TemporaryDirectory() as tmp:
            for micro in UNREADABLE:
                with self.subTest(micro=micro):
                    out = Path(tmp) / f"{micro}.json"
                    run_cli(f"{read_micro(micro)}; write_json {out}")
                    done = subprocess.run(
                        [CLI, "-p", f"read_json {out}"],
                        capture_output=True,
                        text=True,
                        check=False,
                        cwd=REPO_ROOT,
                        env=cli_env(),
                    )
                    self.assertNotEqual(done.returncode, 0, micro)


if __name__ == "__main__":
    unittest.main()

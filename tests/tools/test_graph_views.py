"""The write_graph pass (spec docs/specs/2026-11-05-graph-views-design.md): the state transition
graph, the control flow graph and the signal data dependency graph on small Verilog designs, against
hand-written expected graphs. Run through CTest `graph_views` (ODIN3_BIN set); skipped without it.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path
from typing import Any

ODIN3 = os.environ.get("ODIN3_BIN")

MOORE = """module t(input clk, input rst, input go, output reg o);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule
"""


class GraphTestBase(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)

    def graphs(self, verilog: str, kind: str, pre: str = "proc") -> list[dict[str, Any]]:
        """The graphs of module t after `read_verilog; <pre>; write_graph <kind>`."""
        src = self.dir / "t.v"
        src.write_text(verilog)
        out = self.dir / "g.json"
        steps = [f"read_verilog --nolatches {src}"] + ([pre] if pre else [])
        script = "; ".join(steps + [f"write_graph {kind} {out}"])
        done = subprocess.run([str(ODIN3), "-p", script], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr + done.stdout)
        raw = json.loads(out.read_text())
        self.assertEqual((raw["schema"], raw["version"], raw["kind"]), ("odin3-graph", 1, kind))
        mods = {m["name"]: m for m in raw["modules"]}
        result: list[dict[str, Any]] = mods["t"]["graphs"]
        return result


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest graph_views)")
class StgTest(GraphTestBase):
    def test_moore_three_states(self) -> None:
        graphs = self.graphs(MOORE, "stg")
        self.assertEqual(len(graphs), 1)
        graph = graphs[0]
        self.assertTrue(graph["id"].startswith("fsm:"))
        states = {n["id"]: n for n in graph["nodes"]}
        codes = sorted(n["code"] for n in states.values())
        self.assertEqual(codes, ["00", "01", "10"])
        reset = [n for n in states.values() if n["reset"]]
        self.assertEqual([n["code"] for n in reset], ["00"])
        by_code = {n["code"]: n["id"] for n in states.values()}
        edges = {(e["from"], e["to"], e["cond_expr"]) for e in graph["edges"]}
        want = {
            (by_code["00"], by_code["01"], "go"),
            (by_code["00"], by_code["00"], "!go"),
            (by_code["01"], by_code["10"], "1"),
            (by_code["10"], by_code["00"], "1"),
        }
        self.assertEqual(edges, want)
        self.assertEqual(graph["encoding"], "auto")
        self.assertTrue(graph["src"], "the state register has an HDL span")
        self.assertTrue(graph["select_nodes"])

    def test_no_fsm(self) -> None:
        text = "module t(input a, input b, output y); assign y = a & b; endmodule\n"
        self.assertEqual(self.graphs(text, "stg"), [])

    def test_needs_proc(self) -> None:
        src = self.dir / "t.v"
        src.write_text(MOORE)
        script = f"read_verilog --nolatches {src}; write_graph stg {self.dir / 'g.json'}"
        done = subprocess.run([str(ODIN3), "-p", script], capture_output=True, text=True)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("run proc first", done.stderr + done.stdout)


CASE = """module t(input [1:0] s, input a, input b, output reg y, output reg z);
  always @* begin
    y = 0; z = a;
    case (s)
      2'd0: y = a;
      2'd1, 2'd2: if (b) y = 1; else z = b;
      default: y = b;
    endcase
  end
endmodule
"""

CHAIN = """module t(input a, input b, output reg y);
  always @* if (a) y = 1; else if (b) y = 0; else y = a;
endmodule
"""

TWO = """module t(input clk, input a, output reg p, output reg q);
  always @(posedge clk) p <= a;
  always @* q = p & a;
endmodule
"""


def short(node_id: str) -> str:
    return node_id.split("/", 1)[1]


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest graph_views)")
class CfgTest(GraphTestBase):
    def test_case_statement(self) -> None:
        graphs = self.graphs(CASE, "cfg", pre="")
        self.assertEqual(len(graphs), 1)
        graph = graphs[0]
        label = {short(n["id"]): n["label"] for n in graph["nodes"]}
        self.assertEqual(
            sorted(n["kind"] for n in graph["nodes"]),
            ["case"] * 5 + ["entry", "exit", "join", "join", "switch", "switch"],
        )
        edges = {(short(e["from"]), short(e["to"]), e["kind"], e["label"]) for e in graph["edges"]}
        want = {
            ("entry", "sw0", "seq", ""),
            ("sw0", "c1", "control", "2'00"),
            ("sw0", "c2", "control", "2'01 , 2'10"),
            ("sw0", "c3", "control", "default"),
            ("c1", "j0", "seq", ""),
            ("c2", "sw1", "seq", ""),
            ("sw1", "c4", "control", "1'1"),
            ("sw1", "c5", "control", "default"),
            ("c4", "j1", "seq", ""),
            ("c5", "j1", "seq", ""),
            ("j1", "j0", "seq", ""),
            ("c3", "j0", "seq", ""),
            ("j0", "exit", "seq", ""),
        }
        self.assertEqual(edges, want)
        self.assertEqual(label["sw0"], "switch \\s")
        self.assertEqual(label["sw1"], "switch \\b")
        by_id = {short(n["id"]): n for n in graph["nodes"]}
        self.assertTrue(by_id["c2"]["src"] and by_id["sw0"]["src"], "branches have HDL spans")
        self.assertEqual(by_id["c3"]["default"], True)
        self.assertEqual(by_id["c1"]["actions"][0]["rhs"], "\\a")

    def test_if_else_chain(self) -> None:
        graph = self.graphs(CHAIN, "cfg", pre="")[0]
        kinds = sorted(n["kind"] for n in graph["nodes"])
        want = ["case"] * 4 + ["entry", "exit", "join", "join", "switch", "switch"]
        self.assertEqual(kinds, want)
        defaults = [n for n in graph["nodes"] if n["kind"] == "case" and n["default"]]
        self.assertEqual(len(defaults), 2)

    def test_two_processes(self) -> None:
        graphs = self.graphs(TWO, "cfg", pre="")
        self.assertEqual(len(graphs), 2)
        self.assertEqual(len({g["id"] for g in graphs}), 2)
        types = sorted(s["type"] for g in graphs for s in g["syncs"])
        self.assertEqual(types, ["always", "posedge"])
        for graph in graphs:
            self.assertEqual([e["to"] for e in graph["edges"]], [graph["id"] + "/exit"])


DDG = """module t(input clk, input a, input b, input [1:0] s, output reg q, output reg y);
  wire w = a & b;
  always @(posedge clk) q <= w;
  always @* begin
    y = 0;
    case (s)
      2'd0: y = a;
      default: if (b) y = w;
    endcase
  end
endmodule
"""


def named_edges(graph: dict[str, Any]) -> set[tuple[str, str, str, str, bool]]:
    label = {n["id"]: n["label"] for n in graph["nodes"]}
    return {
        (label[e["from"]], label[e["to"]], e["kind"], e["op"], e["seq"]) for e in graph["edges"]
    }


def reaches(graph: dict[str, Any], src: str, dst: str) -> bool:
    label = {n["id"]: n["label"] for n in graph["nodes"]}
    succ: dict[str, set[str]] = {}
    for e in graph["edges"]:
        succ.setdefault(label[e["from"]], set()).add(label[e["to"]])
    seen, todo = {src}, [src]
    while todo:
        for nxt in succ.get(todo.pop(), ()):
            if nxt not in seen:
                seen.add(nxt)
                todo.append(nxt)
    return dst in seen


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest graph_views)")
class DdgTest(GraphTestBase):
    def test_after_proc(self) -> None:
        graph = self.graphs(DDG, "ddg")[0]
        edges = named_edges(graph)
        for want in [
            ("a", "w", "data", "$and", False),
            ("b", "w", "data", "$and", False),
            ("clk", "q", "control", "$dff", True),
            ("w", "q", "data", "$dff", True),
        ]:
            self.assertIn(want, edges)
        self.assertTrue(reaches(graph, "s", "y"))
        self.assertTrue(reaches(graph, "b", "y"))
        self.assertFalse(reaches(graph, "y", "q"))
        sig = {n["label"]: n for n in graph["nodes"] if n["kind"] == "signal"}
        self.assertEqual(sig["s"]["width"], 2)
        self.assertEqual((sig["s"]["port"], sig["q"]["port"], sig["w"]["port"]), ("in", "out", ""))
        andw = [e for e in graph["edges"] if e["op"] == "$and"]
        self.assertTrue(andw and andw[0]["src"], "the HDL line of the edge")

    def test_before_proc(self) -> None:
        graph = self.graphs(DDG, "ddg", pre="")[0]
        edges = named_edges(graph)
        self.assertIn(("a", "w", "data", "$and", False), edges)
        self.assertTrue(any(e[3] == "$proc" and e[2] == "control" for e in edges))
        self.assertTrue(reaches(graph, "s", "y"))
        self.assertTrue(reaches(graph, "w", "q"))
        self.assertTrue(reaches(graph, "clk", "q"))
        self.assertFalse(reaches(graph, "q", "y"))

    def test_two_always_share(self) -> None:
        graph = self.graphs(TWO, "ddg")[0]
        self.assertTrue(reaches(graph, "a", "p"))
        self.assertTrue(reaches(graph, "p", "q"))
        self.assertTrue(reaches(graph, "a", "q"))
        self.assertFalse(reaches(graph, "q", "p"))


@unittest.skipUnless(ODIN3, "ODIN3_BIN not set (run CTest graph_views)")
class ScoapProvTest(GraphTestBase):
    def run_scoap(self, flags: str) -> dict[str, Any]:
        src = self.dir / "t.v"
        src.write_text(DDG)
        out = self.dir / "s.json"
        script = f"read_verilog --nolatches {src}; proc; scoap {flags} --out {out}"
        done = subprocess.run([str(ODIN3), "-p", script], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr + done.stdout)
        result: dict[str, Any] = json.loads(out.read_text())
        return result

    def test_prov_keys_join_to_ddg(self) -> None:
        plain = self.run_scoap("")
        withp = self.run_scoap("--prov")
        for net in plain["modules"][0]["nets"]:
            self.assertNotIn("driver", net)
        self.assertEqual(
            [
                {k: v for k, v in n.items() if k not in ("driver", "wire", "bit", "prov", "src")}
                for n in withp["modules"][0]["nets"]
            ],
            plain["modules"][0]["nets"],
        )
        graph = self.graphs(DDG, "ddg")[0]
        wire_id = {n["label"]: n["id"] for n in graph["nodes"] if n["kind"] == "signal"}
        by_wire = {f"w{n['wire']}": n for n in withp["modules"][0]["nets"] if n["wire"]}
        net = by_wire[wire_id["w"]]
        self.assertNotEqual(net["driver"], 0)
        self.assertTrue(net["src"], "the net has an HDL span")
        self.assertEqual(net["driver"], [e["via"] for e in graph["edges"] if e["op"] == "$and"][0])


if __name__ == "__main__":
    unittest.main()

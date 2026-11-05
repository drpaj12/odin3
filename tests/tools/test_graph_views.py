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
        script = f"read_verilog --nolatches {src}; {pre}; write_graph {kind} {out}"
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


if __name__ == "__main__":
    unittest.main()

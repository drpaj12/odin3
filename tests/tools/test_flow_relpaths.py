"""odin3-flow names are relative to the design's common directory (after 1.2.0): one design in two
different directories, or given by an absolute or a relative path, gives byte-identical BLIF, name
map and run summary, and none holds a temporary directory path."""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import odin3_flow as flow

from .helpers import run_main

ODIN3 = os.environ.get("ODIN3_CLI", "")
needs_odin3 = unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools)")
TOP = "rp_top"
FILES = {
    "rp.vh": "`define RP_W 4\n",
    "rp_top.v": '`include "rp.vh"\nmodule rp_top(input [`RP_W-1:0] a, input [`RP_W-1:0] b, '
                'output [`RP_W-1:0] y);\n  rp_sub u(.a(a), .b(b), .y(y));\nendmodule\n',
    "lib/rp_sub.v": '`include "../rp.vh"\nmodule rp_sub(input [`RP_W-1:0] a, '
                    'input [`RP_W-1:0] b, output [`RP_W-1:0] y);\n  assign y = a ^ (a & b);\n'
                    'endmodule\n',
}


def make_tree(parent: Path) -> list[Path]:
    """The design files under ``parent``; returns the two .v files."""
    for name, text in FILES.items():
        path = parent / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    return [parent / "rp_top.v", parent / "lib" / "rp_sub.v"]


def env_without_vtr() -> dict[str, str]:
    return {k: v for k, v in os.environ.items() if k != "VTR_ROOT"}


@needs_odin3
class RelativeNames(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)

    def flow_in(self, tree: Path, out: Path, designs: list[str], cwd: Path) -> dict[str, bytes]:
        """Run the flow with ``--prov`` and return the BLIF, name map and its log by file name."""
        old = Path.cwd()
        os.chdir(cwd)
        try:
            with mock.patch.dict(os.environ, env_without_vtr(), clear=True):
                code, text, err = run_main(
                    flow.main, [*designs, "--top", TOP, "--no-vpr", "--prov", "--odin3", ODIN3,
                                "--out", str(out)])
        finally:
            os.chdir(old)
        self.assertEqual(code, 0, text + err)
        step = out / "odin3"
        return {n: (step / n).read_bytes() for n in (f"{TOP}.blif", f"{TOP}.blif.names")}

    def test_two_directories_give_identical_outputs(self) -> None:
        results: list[dict[str, bytes]] = []
        for sub in ("a", "deeper/than/the/first/b"):
            tree = self.tmp / sub
            files = make_tree(tree)
            results.append(self.flow_in(tree, self.tmp / f"out_{len(results)}",
                                        [str(f) for f in files], self.tmp))
            for name, data in results[-1].items():
                self.assertNotIn(str(self.tmp).encode(), data, name)
        self.assertEqual(results[0], results[1])

    def test_absolute_and_relative_designs_agree(self) -> None:
        tree = self.tmp / "t"
        files = make_tree(tree)
        absolute = self.flow_in(tree, self.tmp / "out_abs", [str(f) for f in files], self.tmp)
        relative = self.flow_in(tree, self.tmp / "out_rel",
                                [os.path.relpath(f, tree) for f in files], tree)
        self.assertEqual(absolute, relative)

    def test_name_root_logged_without_absolute_path(self) -> None:
        tree = self.tmp / "t"
        files = make_tree(tree)
        self.flow_in(tree, self.tmp / "out", [str(f) for f in files], self.tmp)
        log = (self.tmp / "out" / "odin3" / "odin3.log").read_text()
        line = next(ln for ln in log.splitlines() if ln.startswith("name_root="))
        self.assertEqual(line, "name_root=. designs=rp_top.v lib/rp_sub.v")

    def test_design_root_is_common_parent(self) -> None:
        designs = [Path("/x/y/a.v"), Path("/x/y/lib/b.v")]
        self.assertEqual(flow.design_root(designs), Path("/x/y"))
        self.assertEqual(flow.design_root(designs[:1]), Path("/x/y"))


if __name__ == "__main__":
    unittest.main()

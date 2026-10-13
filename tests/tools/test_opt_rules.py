"""Tests for tools/opt-check/opt-rules (2F Task 16): the per-rule golden runner.

Fakes stand in for both tools. The fake ``odin3`` reads ``fake.json`` beside it: it writes ``in``
to the file after ``write_rtlil`` the first time and ``out`` the second time, and prints ``log``
(the ``--opt-stats``, INFO and ``stats`` lines). The fake ``yosys`` prints ``ystat`` and writes
``cover`` into ``YOSYS_COVER_DIR``. Both log their ``-p`` script, so the command strings can be
read back. ``memcap.run`` is replaced by a plain subprocess call.
"""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any
from unittest import mock

import memcap
import opt_rules as orr

from . import helpers

FAKE_ODIN = r"""#!/usr/bin/env python3
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
script = sys.argv[sys.argv.index("-p") + 1]
with open(HERE / "odin.log", "a") as log:
    log.write(script + "\n")
paths = [c.split()[1] for c in script.split("; ") if c.startswith("write_rtlil")]
Path(paths[0]).write_text(spec["in"])
Path(paths[1]).write_text(spec["out"])
sys.stderr.write(spec["log"])
sys.exit(spec.get("exit", 0))
"""

FAKE_YOSYS = r"""#!/usr/bin/env python3
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = json.loads((HERE / "fake.json").read_text())
with open(HERE / "yosys.log", "a") as log:
    log.write(sys.argv[sys.argv.index("-p") + 1] + "\n")
(Path(os.environ["YOSYS_COVER_DIR"]) / "cover.txt").write_text(spec["cover"])
sys.stdout.write(spec["ystat"])
"""

ODIN_LOG = """odin3: info: opt: rule const_and 1
odin3: info: stats: module t: cell $port_in 1
odin3: info: stats: module t: cell $_CONST0_ 1
odin3: info: stats: module t: cell $xor 2
"""
YSTAT = """
=== t ===

   Number of cells:                  2
     $xor                            2
"""
COVER = "passes/opt/opt_expr.cc:570:replace_const_cells   1 opt.opt_expr.const_and\n"
SOURCE = ("// expr/const_and: AND with a zero\n// options: -fine\n"
          "module t(input a, output y);\nendmodule\n")


def fake_run(argv: Sequence[str], cap: int, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    """Stands in for memcap.run: no cap, the child's cwd and env as given."""
    del cap
    return subprocess.run(list(argv), capture_output=True, text=True, check=False,
                          cwd=kwargs.get("cwd"), env=kwargs.get("env"))


class Tree:
    """A temporary rules tree with fakes."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.bin = root / "bin"
        self.bin.mkdir()
        self.rules = root / "rules" / "rules.txt"
        (root / "rules" / "expr").mkdir(parents=True)
        for name, text in (("odin3", FAKE_ODIN), ("yosys", FAKE_YOSYS)):
            path = self.bin / name
            path.write_text(text)
            path.chmod(0o755)
        self.set_spec()

    def set_spec(self, **changes: Any) -> None:
        spec = {"in": "IN\n", "out": "OUT\n", "log": ODIN_LOG, "ystat": YSTAT, "cover": COVER}
        spec.update(changes)
        (self.bin / "fake.json").write_text(json.dumps(spec))

    def rule(self, name: str, row: str, *, golden: str | None = "OUT\n", source: str = SOURCE
             ) -> None:
        sub, leaf = name.split("/")
        base = self.root / "rules" / sub / leaf
        base.with_name(base.name + ".v").write_text(source)
        if golden is not None:
            base.with_name(base.name + ".il").write_text(golden)
        with open(self.rules, "a", encoding="utf-8") as handle:
            handle.write(row + "\n")

    def run(self, *extra: str, yosys: bool = True) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.bin / "odin3"), "--rules", str(self.rules),
                "--work", str(self.root / "work"), *(["--yosys", str(self.bin / "yosys")]
                                                     if yosys else []), *extra,
                str(self.root / "rules" / "expr")]
        with mock.patch.object(memcap, "run", fake_run):
            return helpers.run_main(orr.main, argv)

    def golden(self, leaf: str) -> str:
        return (self.root / "rules" / "expr" / f"{leaf}.il").read_text()

    def log(self, name: str) -> list[str]:
        return (self.bin / name).read_text().splitlines()


class OptRulesTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tree = Tree(Path(self._tmp.name))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def good(self, extra: str = "") -> None:
        self.tree.rule("expr/const_and", f"expr/const_and const_and opt.opt_expr.const_and {extra}")

    def test_command_strings(self) -> None:
        self.good()
        code, out, _ = self.tree.run()
        self.assertEqual(code, 0, out)
        work = (self.tree.root / "work" / "expr_const_and").resolve()
        src = (self.tree.root / "rules" / "expr" / "const_and.v").resolve()
        self.assertEqual(self.tree.log("odin.log"), [
            f"read_verilog --nolatches {src}; proc --noopt; write_rtlil {work}/in.il; "
            f"opt_expr -fine --opt-stats; stats; write_rtlil {work}/out.il"])
        self.assertEqual(self.tree.log("yosys.log"),
                         [f"read_rtlil {work}/in.il; debug opt_expr -fine; stat"])

    def test_blif_source_command(self) -> None:
        rule = orr.Rule("expr/gate_not", "gate_not", "-")
        script = orr.odin_script(rule, Path("/x/g.blif"), [], Path("/w/in.il"), Path("/w/out.il"))
        self.assertTrue(script.startswith("read_blif /x/g.blif; write_rtlil"))
        self.assertEqual(orr.source_options("# options: -fine -full\n"), ["-fine", "-full"])

    def test_prepass_runs_before_the_input_is_written(self) -> None:
        rule = orr.Rule("dff/x", "c", "-")
        prepass = orr.source_prepass("// dff/x: d\n// prepass: opt_dff; opt_expr\nmodule t;\n")
        self.assertEqual(prepass, ["opt_dff", "opt_expr"])
        script = orr.odin_script(rule, Path("/x/g.v"), [], Path("/w/in.il"), Path("/w/out.il"),
                                 prepass)
        self.assertEqual(script, "read_verilog --nolatches /x/g.v; proc --noopt; opt_dff; "
                                 "opt_expr; write_rtlil /w/in.il; opt_dff --opt-stats; stats; "
                                 "write_rtlil /w/out.il")
        self.assertEqual(orr.source_prepass("module t;\n"), [])

    def test_pass_and_golden_match(self) -> None:
        self.good()
        code, out, _ = self.tree.run()
        self.assertEqual(code, 0, out)
        self.assertIn("expr/const_and: ok", out)
        self.assertIn("1 rules: 1 ok, 0 pending, 0 failed", out)

    def test_zero_counter_fails(self) -> None:
        self.good()
        self.tree.set_spec(log=ODIN_LOG.replace("const_and 1", "const_and 0"))
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("counter const_and is 0, want at least 1", out)

    def test_missing_cover_id_fails_and_names_what_fired(self) -> None:
        self.good()
        zeroed = COVER.replace("1 opt.opt_expr.const_and", "0 opt.opt_expr.const_and")
        self.tree.set_spec(cover=zeroed + "x.cc:1:f   2 opt.opt_expr.const_or\n")
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("yosys cover file has no count for opt.opt_expr.const_and", out)
        self.assertIn("opt.opt_expr.const_or", out)

    def test_log_pattern_row(self) -> None:
        self.tree.rule("expr/const_and", r"expr/const_and const_and log:Inverting\sCLK")
        self.tree.set_spec(ystat=YSTAT + "Inverting CLK of $dff\n")
        self.assertEqual(self.tree.run()[0], 0)
        self.tree.set_spec(ystat=YSTAT)
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("yosys log has no match", out)

    def test_histogram_difference_fails(self) -> None:
        self.good()
        self.tree.set_spec(ystat=YSTAT.replace("$xor                            2",
                                               "$xor                            3"))
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("cell histogram differs", out)
        self.assertIn("module 't'", out)

    def test_golden_difference_fails(self) -> None:
        self.tree.rule("expr/const_and", "expr/const_and const_and opt.opt_expr.const_and",
                       golden="OTHER\n")
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("out.il differs from const_and.il", out)

    def test_odin_failure_fails(self) -> None:
        self.good()
        self.tree.set_spec(exit=1)
        code, out, _ = self.tree.run()
        self.assertEqual(code, 1)
        self.assertIn("odin3 exit 1", out)

    def test_pending_rows_are_listed_not_failed(self) -> None:
        self.good()
        self.tree.rule("expr/later", "expr/later const_or opt.opt_expr.const_or pending",
                       golden=None)
        code, out, _ = self.tree.run()
        self.assertEqual(code, 0, out)
        self.assertIn("expr/later: pending", out)
        self.assertIn("2 rules: 1 ok, 1 pending, 0 failed", out)

    def test_write_refused_on_any_failure(self) -> None:
        self.good()
        self.tree.rule("expr/second", "expr/second const_or opt.opt_expr.const_or", golden=None)
        self.tree.set_spec(out="NEW\n")  # const_or has no count in COVER: second fails
        code, out, _ = self.tree.run("--write")
        self.assertEqual(code, 1)
        self.assertIn("--write refused", out)
        self.assertEqual(self.tree.golden("const_and"), "OUT\n")  # untouched
        self.assertFalse((self.tree.root / "rules" / "expr" / "second.il").exists())

    def test_write_writes_when_all_pass(self) -> None:
        self.tree.rule("expr/const_and", "expr/const_and const_and opt.opt_expr.const_and",
                       golden=None)
        self.tree.set_spec(out="NEW\n")
        code, out, _ = self.tree.run("--write")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.tree.golden("const_and"), "NEW\n")

    def test_write_needs_yosys(self) -> None:
        self.good()
        code, _, err = self.tree.run("--write", yosys=False)
        self.assertEqual(code, 2)
        self.assertIn("--write needs --yosys", err)
        self.assertEqual(self.tree.golden("const_and"), "OUT\n")

    def test_no_yosys_checks_counter_and_golden_only(self) -> None:
        self.good()
        code, out, _ = self.tree.run(yosys=False)
        self.assertEqual(code, 0, out)
        self.assertFalse((self.tree.bin / "yosys.log").exists())

    def test_deviation_rows(self) -> None:
        self.good("dev=2")
        # row 2: Yosys's `$pos` is ignored, other cell types still compare
        self.tree.set_spec(ystat=YSTAT + "     $pos                            1\n")
        self.assertEqual(self.tree.run()[0], 0)
        rule = orr.Rule("expr/x", "c", "-", "dev=24")
        self.assertEqual(orr.yosys_script(rule, ["-mux_undef"], Path("/w/in.il")),
                         "read_rtlil /w/in.il; debug opt_expr -mux_undef; "
                         "opt_clean; debug opt_expr -mux_undef; stat")
        other = orr.Rule("expr/x", "c", "-", "dev=7")
        self.assertEqual(orr.yosys_script(other, [], Path("/w/in.il")),
                         "read_rtlil /w/in.il; debug opt_expr; stat")

    def test_other_deviation_is_reported_not_failed(self) -> None:
        self.good("dev=7")
        self.tree.set_spec(ystat=YSTAT.replace("2\n", "5\n", 2))
        code, out, _ = self.tree.run()
        self.assertEqual(code, 0, out)
        self.assertIn("deviation 7: cell histogram differs", out)

    def test_info_counters(self) -> None:
        log = ("odin3: info: opt_merge: removed 3 cells\n"
               "odin3: info: opt_dff: 2 flip-flops changed, 5 constant bits\n")
        self.assertEqual(orr.counter_value(orr.Rule("merge/a", "merge.removed", "-"), log), 3)
        self.assertEqual(orr.counter_value(orr.Rule("dff/a", "dff.constbits", "-"), log), 5)
        self.assertEqual(orr.counter_value(orr.Rule("dff/a", "opt_dff.d_eq_q", "-"),
                                           "odin3: info: opt: rule opt_dff.d_eq_q 4\n"), 4)

    def test_orphan_source_is_listed(self) -> None:
        self.good()
        (self.tree.root / "rules" / "expr" / "stray.v").write_text("module s; endmodule\n")
        _, out, _ = self.tree.run()
        self.assertIn("expr/stray: not listed in rules.txt", out)

    def test_rules_file_errors(self) -> None:
        with self.assertRaises(orr.UsageError):
            orr.parse_rules("expr/a only_two\n")
        with self.assertRaises(orr.UsageError):
            orr.parse_rules("bogus/a c -\n")
        with self.assertRaises(orr.UsageError):
            orr.parse_rules("expr/a c - later\n")
        with self.assertRaises(orr.UsageError):
            orr.parse_rules("expr/a c -\nexpr/a c -\n")
        rows = orr.parse_rules("# comment\nexpr/a c - pending  # why\nexpr/b c opt.x dev=2\n")
        self.assertEqual([r.name for r in rows], ["expr/a", "expr/b"])
        self.assertTrue(rows[0].pending)
        self.assertEqual(rows[1].deviation, "2")

    def test_histogram_parsers(self) -> None:
        self.assertEqual(orr.odin_histogram(ODIN_LOG), {"t": {"$xor": 2}})
        self.assertEqual(orr.yosys_histogram(YSTAT), {"t": {"$xor": 2}})
        design = YSTAT + "\n=== design hierarchy ===\n\n   Number of cells:   2\n     $xor   2\n"
        self.assertEqual(orr.yosys_histogram(design), {"t": {"$xor": 2}})
        self.assertEqual(orr.odin_histogram("stats: module m: ports 3, nodes 1\n"), {"m": {}})


if __name__ == "__main__":
    unittest.main()

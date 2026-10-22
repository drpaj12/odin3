"""Tests for tools/gatemap-check/gatemap-run (gatemap plan Task 14): the cell and provenance
checks on fixture dumps, the scripts, and the outcome and exit code of each leg with fake
``odin3`` and ``equiv-check`` scripts standing in for the real ones."""

from __future__ import annotations

import subprocess
import unittest
from collections.abc import Sequence
from pathlib import Path
from typing import Any
from unittest import mock

import gatemap_run as gr
import memcap

from . import helpers

DATA = Path(__file__).resolve().parent / "data" / "gatemap"
CELLS = {"NAND2", "DFF"}


def fake_run(argv: Sequence[str], cap: int, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    del cap
    return subprocess.run(list(argv), capture_output=True, text=True, check=False,
                          cwd=kwargs.get("cwd"))


class CheckTest(unittest.TestCase):
    def locs(self, name: str) -> str:
        return (DATA / name).read_text(encoding="utf-8")

    def test_library_cells_are_read(self) -> None:
        self.assertEqual(gr.library_cells((DATA / "lib.o3lib").read_text()), CELLS)

    def test_allowed_types_ok(self) -> None:
        self.assertEqual(gr.judge_cells(self.locs("locs_ok.txt"), CELLS), ("ok", ""))

    def test_foreign_cell_named(self) -> None:
        kind, detail = gr.judge_cells(self.locs("locs_foreign.txt"), CELLS)
        self.assertEqual(kind, "foreign-cell")
        self.assertIn("$lut", detail)

    def test_noprov_named(self) -> None:
        kind, detail = gr.judge_cells(self.locs("locs_noprov.txt"), CELLS)
        self.assertEqual(kind, "noprov")
        self.assertIn("DFF", detail)

    def test_the_first_script_is_the_gate_flow(self) -> None:
        script = gr.flow_script(["a.v"], None, ["/l/vtr.o3lib", "/l/cmos6.o3lib"], "cmos6",
                                Path("/w"))
        steps = script.split("; ")
        self.assertEqual(steps[:2], ["read_techlib /l/vtr.o3lib", "read_techlib /l/cmos6.o3lib"])
        self.assertEqual(steps[2], "read_verilog --nolatches a.v")
        self.assertEqual(steps[4:8], ["proc", "opt", "lower --neg-edge keep",
                                      "write_blif --gates-as-names /w/rtl.blif"])
        self.assertEqual(steps[8:10], ["abc --gates cmos6", "dfflibmap cmos6"])
        self.assertEqual(steps[11], "write_blif /w/cells.blif")

    def test_the_remap_script_reads_the_netlist(self) -> None:
        script = gr.remap_script(["/l/g.o3lib"], "g", Path("/w"))
        self.assertIn("read_blif /w/cells.blif", script)
        self.assertTrue(script.endswith("write_blif --gates-as-names /w/remap.blif"))

    def test_outcome_words_and_exit_codes(self) -> None:
        self.assertEqual([gr.equiv_outcome(c) for c in (0, 1, 2, 4, 5)],
                         ["ok", "differs", "error", "unproven", "unproven"])
        self.assertEqual([gr.exit_code(o) for o in
                          ("ok", "differs", "error", "foreign-cell", "noprov", "unproven",
                           "remap-differs", "remap-unproven")], [0, 1, 1, 1, 1, 4, 1, 4])


class RunTest(unittest.TestCase):
    def setUp(self) -> None:
        self.root = helpers.scratch(self, "gatemap_run_test_")
        (self.root / "m.v").write_text("module m(input a, output y); endmodule\n")

    def fake(self, name: str, body: str) -> Path:
        path = self.root / name
        path.write_text("#!/usr/bin/env bash\n" + body)
        path.chmod(0o755)
        return path

    def run_fake(self, odin3: str, equiv: str, *extra: str) -> tuple[int, str, str]:
        work = self.root / "work"
        argv = ["--odin3", str(self.fake("odin3", odin3)),
                "--equiv-check", str(self.fake("equiv", equiv)),
                "--lib", f"tiny={DATA / 'lib.o3lib'}", "--work", str(work), *extra,
                str(self.root / "m.v")]
        with mock.patch.object(memcap, "run", fake_run):
            return helpers.run_main(gr.main, argv)

    # the fake odin3 runs in the work directory and writes the dump and the BLIF files there
    ODIN = ("work=$PWD\n"
            "cp %s \"$work/locs.txt\" 2>/dev/null || true\n"
            "for f in rtl gates cells remap; do echo '.model t' > \"$work/$f.blif\"; done\n")

    def odin(self, locs: str) -> str:
        return self.ODIN % (DATA / locs)

    def test_ok_counts_as_compared(self) -> None:
        code, out, err = self.run_fake(self.odin("locs_ok.txt"), "exit 0\n")
        self.assertEqual(code, 0, out + err)
        self.assertIn("gatemap-run: ok ", out)
        self.assertIn("gatemap-run: compared 1 of 1 designs", out)

    def test_differs_is_exit_1(self) -> None:
        code, out, _ = self.run_fake(self.odin("locs_ok.txt"), "echo NOT equivalent\nexit 1\n")
        self.assertEqual(code, 1)
        self.assertIn("gatemap-run: differs ", out)
        self.assertIn("compared 0 of 1", out)

    def test_unproven_is_exit_4(self) -> None:
        code, out, _ = self.run_fake(self.odin("locs_ok.txt"), "exit 4\n")
        self.assertEqual(code, 4)
        self.assertIn("gatemap-run: unproven ", out)

    def test_equiv_killed_is_unproven(self) -> None:
        code, out, _ = self.run_fake(self.odin("locs_ok.txt"), "kill -KILL $$\n")
        self.assertEqual(code, 4)
        self.assertIn("gatemap-run: unproven ", out)
        self.assertIn("killed by the memory cap", out)

    def test_equiv_error_is_exit_1(self) -> None:
        code, out, _ = self.run_fake(self.odin("locs_ok.txt"),
                                     "echo 'equiv-check: error: clocked by internal net'\nexit 2\n")
        self.assertEqual(code, 1)
        self.assertIn("gatemap-run: error ", out)

    def test_foreign_cell_is_exit_1(self) -> None:
        code, out, _ = self.run_fake(self.odin("locs_foreign.txt"), "exit 0\n")
        self.assertEqual(code, 1)
        self.assertIn("gatemap-run: foreign-cell ", out)
        self.assertIn("$lut", out)

    def test_noprov_is_exit_1(self) -> None:
        code, out, _ = self.run_fake(self.odin("locs_noprov.txt"), "exit 0\n")
        self.assertEqual(code, 1)
        self.assertIn("gatemap-run: noprov ", out)

    def test_odin3_failure_is_exit_1(self) -> None:
        code, out, _ = self.run_fake("echo 'odin3: error: boom'\nexit 1\n", "exit 0\n")
        self.assertEqual(code, 1)
        self.assertIn("gatemap-run: FAIL odin3", out)

    def test_odin3_signal_is_exit_3(self) -> None:
        code, out, _ = self.run_fake("kill -SEGV $$\n", "exit 0\n")
        self.assertEqual(code, 3)
        self.assertIn("gatemap-run: CRASH odin3", out)
        self.assertIn("signal 11", out)

    def test_equiv_detail_drops_notices_and_paths(self) -> None:
        text = ("equiv-check: notice: /t/x/rtl.blif: 2 latches with init 2/3\n"
                "equiv-check: /t/x/rtl.eq.blif:7: unsupported: latch q is clocked by internal\n")
        self.assertEqual(gr.equiv_detail(text),
                         "equiv-check: rtl.blif:7: unsupported: latch q is clocked by internal")

    def test_hard_blocks_become_gate_lines(self) -> None:
        text = (".model t\n.subckt multiply a[0]=x out[0]=y \\\n  b[0]=z\n.cname m1\n"
                ".subckt NAND2 A=a B=b Y=c\n.subckt adder_x a=b\n.end\n")
        self.assertEqual(gr.hard_as_gate(text),
                         text.replace(".subckt multiply", ".gate multiply"))

    def test_techlib_cells_are_allowed(self) -> None:
        cells = gr.library_cells((DATA / "hard.o3lib").read_text()) | CELLS
        locs = (DATA / "locs_ok.txt").read_text() + "top|node:single_port_ram|m|a.v:6\n"
        self.assertEqual(gr.judge_cells(locs, cells), ("ok", ""))
        self.assertEqual(gr.judge_cells(locs, CELLS)[0], "foreign-cell")
        code, out, err = self.run_fake(self.odin("locs_ok.txt"), "exit 0\n",
                                       "--techlib", str(DATA / "hard.o3lib"))
        self.assertEqual(code, 0, out + err)

    def test_undriven_is_retried_with_allow_undriven(self) -> None:
        equiv = ("case \"$*\" in *--allow-undriven*) exit 0;; esac\n"
                 "echo 'equiv-check: rtl.blif: undriven net a reaches primary output y'\nexit 2\n")
        code, out, err = self.run_fake(self.odin("locs_ok.txt"), equiv)
        self.assertEqual(code, 0, out + err)

    def test_undriven_in_the_right_file_is_not_retried(self) -> None:
        equiv = ("case \"$*\" in *--allow-undriven*) exit 0;; esac\n"
                 "echo \"equiv-check: error: $PWD/gates.eq.blif: undriven net a reaches "
                 "primary output y; pass --allow-undriven to tie it to 0\"\nexit 2\n")
        code, out, _ = self.run_fake(self.odin("locs_ok.txt"), equiv)
        self.assertEqual(code, 1)
        self.assertIn("gatemap-run: error ", out)

    def test_undriven_in_the_left_file_is_retried_in_the_real_form(self) -> None:
        equiv = ("case \"$*\" in *--allow-undriven*) exit 0;; esac\n"
                 "echo \"equiv-check: error: $PWD/rtl.eq.blif: undriven net a reaches "
                 "primary output y; pass --allow-undriven to tie it to 0\"\nexit 2\n")
        code, out, err = self.run_fake(self.odin("locs_ok.txt"), equiv)
        self.assertEqual(code, 0, out + err)

    def test_equiv_is_run_with_the_hard_block_models(self) -> None:
        equiv = "echo \"$@\" > \"$(dirname \"$0\")/args\"\nexit 0\n"
        code, out, err = self.run_fake(self.odin("locs_ok.txt"), equiv)
        self.assertEqual(code, 0, out + err)
        self.assertTrue((self.root / "args").read_text().startswith(
            "--blackbox adder,multiply,single_port_ram,dual_port_ram "))

    def test_remap_runs_after_a_good_first_leg(self) -> None:
        equiv = "echo \"$@\" >> \"$(dirname \"$0\")/calls\"\nexit 0\n"
        code, out, err = self.run_fake(self.odin("locs_ok.txt"), equiv, "--remap")
        self.assertEqual(code, 0, out + err)
        calls = (self.root / "calls").read_text().splitlines()
        self.assertEqual(len(calls), 2)
        remap = self.root / "work" / "remap.eq.blif"
        self.assertTrue(calls[1].endswith(f"gates.eq.blif {remap}"))

    def test_remap_difference_is_named(self) -> None:
        equiv = ("n=$(dirname \"$0\")/n\necho x >> \"$n\"\n"
                 "if [ $(wc -l < \"$n\") -ge 2 ]; then exit 1; fi\nexit 0\n")
        code, out, _ = self.run_fake(self.odin("locs_ok.txt"), equiv, "--remap")
        self.assertEqual(code, 1)
        self.assertIn("gatemap-run: remap-differs ", out)


if __name__ == "__main__":
    unittest.main()

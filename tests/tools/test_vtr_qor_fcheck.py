"""Tests for the fcheck verdicts and the runner wrapper (3H spec §8.1; ``qor_fcheck_run.py``).

The first real fcheck of the 19 reported ``fail`` for every design in seconds: VTR's
``run_random_check.py`` exits 1 both for a vector mismatch and for a Verilator compile failure,
and the compile failed on a duplicate port declaration its own packing leaves behind.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import qor_fcheck_run
import qor_legs
from qor_heavy import HEAVY_DIR

# What Yosys writes for a top with a constant-driven output (ch_intrinsics, 3c9a4d23b27d), and
# what upstream's packing used to turn into "wire memory_controller_address[26] ;" beside the bus.
YOSYS_TOP = r"""module memset(\m~0 , \memory_controller_address~26 ,
  \memory_controller_address~25 );
  input \m~0 ;
  output \memory_controller_address~26 ;
  wire \memory_controller_address~26 ;
  output \memory_controller_address~25 ;
  wire \memory_controller_address~25 ;
  wire \inner~1 ;
  assign \memory_controller_address~26  = 1'h0;
endmodule
"""

COMPILE_ERROR = """\
%Error: dut_synth.v:233:8: Duplicate declaration of signal: 'memory_controller_address[26]'
ERROR: verilator compile (see /x/verilator.out)
tops: synth=memset rtl=memset
"""
MISMATCH = """\
MISMATCH vec=0 port=memory_controller_write_enable rtl=0 synth=1
aborting after 21 errors
VCHECK_SUMMARY matched=0 mismatched=7 vectors=7 port_errors=21
"""
PASSED = ("tops: synth=memset rtl=memset\nPASS: 50000 vectors matched across rtl/post-synth\n"
          "VCHECK_SUMMARY matched=50000 mismatched=0 vectors=50000 port_errors=0\n")


class VerdictTest(unittest.TestCase):
    def test_compile_failure_is_error_not_fail(self) -> None:
        """Exit 1 from a compile failure is ``error``; exit 1 with mismatches is ``fail``."""
        self.assertEqual(qor_legs.fcheck_verdict(1, False, COMPILE_ERROR), "error")
        self.assertEqual(qor_legs.fcheck_verdict(1, False, MISMATCH), "fail")
        # the testbench's $stop after the mismatches exits 250 in practice
        self.assertEqual(qor_legs.fcheck_verdict(250, False, MISMATCH), "fail")
        self.assertEqual(qor_legs.fcheck_verdict(1, False, "FAIL: 3 mismatches in 100 vectors"),
                         "fail")

    def test_pass_needs_the_pass_line(self) -> None:
        self.assertEqual(qor_legs.fcheck_verdict(0, False, PASSED), "pass")
        # exit 0 without it (a checker that did nothing) is never a pass
        self.assertEqual(qor_legs.fcheck_verdict(0, False, ""), "error")
        self.assertEqual(qor_legs.fcheck_verdict(0, False, COMPILE_ERROR), "error")
        # a pass line with a non-zero exit is not a pass either
        self.assertEqual(qor_legs.fcheck_verdict(1, False, PASSED), "error")

    def test_vacuous_pass_is_error(self) -> None:
        """A PASS line over zero compared vectors (no output port) or without the summary."""
        line = "PASS: 0 vectors matched across rtl/post-synth\n"
        for summary in ("VCHECK_SUMMARY matched=0 mismatched=0 vectors=0 port_errors=0\n", ""):
            self.assertEqual(qor_legs.fcheck_verdict(0, False, line + summary), "error")
        self.assertEqual(qor_legs.fcheck_verdict(
            0, False, "PASS: 9 vectors matched\n"
            "VCHECK_SUMMARY matched=9 mismatched=0 vectors=9 port_errors=0\n"), "pass")

    def test_other_exits_and_the_cap(self) -> None:
        for code in (2, 3, 137):
            self.assertEqual(qor_legs.fcheck_verdict(code, False, ""), "error")
        self.assertEqual(qor_legs.fcheck_verdict(137, True, MISMATCH), "capped")


class WrapperTest(unittest.TestCase):
    def test_drop_port_wires(self) -> None:
        text = qor_fcheck_run.drop_port_wires(YOSYS_TOP)
        self.assertNotIn(r"wire \memory_controller_address~26", text)
        self.assertNotIn(r"wire \memory_controller_address~25", text)
        self.assertIn(r"output \memory_controller_address~26 ;", text)
        self.assertIn(r"wire \inner~1 ;", text)  # a wire that is not a port stays
        self.assertIn("assign", text)
        self.assertEqual(qor_fcheck_run.drop_port_wires(text), text)

    def test_wrapper_patches_the_packing_and_keeps_the_exit_code(self) -> None:
        """A stand-in VTR directory: its ``blif_to_verilog`` is the buggy kind (it fails on a
        remaining port wire) and its ``run_random_check.py`` exits with the packing's verdict."""
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as tmp:
            vdir = Path(tmp) / "verilator_check"
            vdir.mkdir()
            sample = Path(tmp) / "top.v"
            sample.write_text(YOSYS_TOP, encoding="utf-8")
            (vdir / "blif_to_verilog.py").write_text(
                "def pack_bit_blasted_top_ports(text):\n"
                "    return text.replace('~', '_')\n", encoding="utf-8")
            (vdir / "run_random_check.py").write_text(
                "import sys\nimport blif_to_verilog\n"
                "text = blif_to_verilog.pack_bit_blasted_top_ports(open(sys.argv[1]).read())\n"
                "dup = 'wire \\\\memory_controller_address_26' in text\n"
                "print('ERROR duplicate' if dup else 'PASS: 1 vectors matched')\n"
                "raise SystemExit(7 if dup else 0)\n", encoding="utf-8")
            run = [sys.executable, "-I", "-B", str(qor_legs.FCHECK_RUNNER),
                   str(vdir / "run_random_check.py"), str(sample)]
            done = subprocess.run(run, capture_output=True, text=True, check=False)
            self.assertEqual((done.returncode, done.stdout.strip()),
                             (0, "PASS: 1 vectors matched"), done.stderr)
            # nothing is written next to the VTR scripts (-B)
            self.assertEqual(sorted(p.name for p in vdir.iterdir()),
                             ["blif_to_verilog.py", "run_random_check.py"])
            # the unpatched script path fails with the same input (the test sees the bug)
            plain = subprocess.run([sys.executable, "-B", str(vdir / "run_random_check.py"),
                                    str(sample)], capture_output=True, text=True, check=False)
            self.assertEqual(plain.returncode, 7)

    def test_only_the_top_module_is_filtered(self) -> None:
        two = YOSYS_TOP + (r"module sub(\x~1 );" "\n" r"  input \x~1 ;" "\n" r"  wire \x~1 ;"
                           "\nendmodule\n")
        text = qor_fcheck_run.drop_port_wires(two)
        self.assertEqual(text.count(r"wire \x~1 ;"), 1)  # sub keeps its own line
        self.assertNotIn(r"wire \memory_controller_address~26", text)

    def test_str_exit_message_and_missing_function(self) -> None:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as tmp:
            vdir = Path(tmp)
            (vdir / "blif_to_verilog.py").write_text("def pack_bit_blasted_top_ports(t):\n"
                                                     "    return t\n", encoding="utf-8")
            (vdir / "run_random_check.py").write_text("raise SystemExit('boom message')\n",
                                                      encoding="utf-8")
            run = [sys.executable, "-I", "-B", str(qor_legs.FCHECK_RUNNER),
                   str(vdir / "run_random_check.py")]
            done = subprocess.run(run, capture_output=True, text=True, check=False)
            self.assertEqual((done.returncode, done.stderr.strip()), (1, "boom message"))
            (vdir / "blif_to_verilog.py").write_text("def renamed(t):\n    return t\n",
                                                     encoding="utf-8")
            done = subprocess.run(run, capture_output=True, text=True, check=False)
            self.assertEqual(done.returncode, 2)
            self.assertIn("has no pack_bit_blasted_top_ports (renamed or removed upstream)",
                          done.stderr)

    def test_wrapper_usage_error(self) -> None:
        done = subprocess.run([sys.executable, "-I", "-B", str(qor_legs.FCHECK_RUNNER)],
                              capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 2)


if __name__ == "__main__":
    unittest.main()

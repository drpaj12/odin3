"""Tests for the fcheck verdicts and the runner wrapper (3H spec §8.1; ``qor_fcheck_run.py``).

The first real fcheck of the 19 reported ``fail`` for every design in seconds: VTR's
``run_random_check.py`` exits 1 both for a vector mismatch and for a Verilator compile failure,
and the compile failed on a duplicate port declaration its own packing leaves behind.
"""

from __future__ import annotations

import os
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


# The shape of VTR's generated testbench around what the wrapper edits (tb_generator.py).
TB = """\
module tb;
    localparam int NUM_VECTORS = 50000;
    localparam int MAX_ERRORS = 20;
    initial begin
        for (vecIdx = 0; vecIdx < NUM_VECTORS; vecIdx++) begin
            if (q_rtl !== q_synth) begin
                if (errors < MAX_ERRORS) begin
                    $display("MISMATCH vec=%0d port=q", vecIdx);
                end
                errors++;
                vecFail = 1;
            end
            if (vecFail) begin
                vecMismatched++;
            end else begin
                vecMatched++;
            end
            if (errors >= MAX_ERRORS) begin
                $display("aborting after %0d errors", errors);
                $display("VCHECK_SUMMARY matched=%0d", vecMatched);
                $fatal(1);
            end
        end
        $display("VCHECK_SUMMARY matched=%0d", vecMatched);
        if (errors == 0) begin
            $display("PASS: %0d vectors matched across rtl/post-synth", NUM_VECTORS);
            $finish;
        end else begin
            $display("FAIL: %0d mismatches in %0d vectors", errors, NUM_VECTORS);
            $fatal(1);
        end
    end
endmodule
"""
OUTPUTS = [("q", 3), ("done", 1)]


def vd(*pairs: tuple[int, int]) -> str:
    """One dump line of the instrumented testbench: ``rtl:synth`` per port, in hex."""
    return "VD " + " ".join(f"{r:x}:{s:x}" for r, s in pairs) + "\n"


class InstrumentTest(unittest.TestCase):
    def test_dump_limit_and_renamed_verdicts(self) -> None:
        out = qor_fcheck_run.instrument_testbench(TB, OUTPUTS)
        dump = '$display("VD %h:%h %h:%h", q_rtl, q_synth, done_rtl, done_synth);'
        self.assertEqual(out.count(dump), 1)
        self.assertLess(out.index(dump), out.index("if (vecFail) begin"))
        self.assertIn("MAX_ERRORS = 2147483647;", out)
        self.assertNotIn("MAX_ERRORS = 20;", out)
        self.assertNotIn("if (errors < MAX_ERRORS)", out)  # no mismatch line of its own
        self.assertNotIn("$fatal", out)
        # only the wrapper writes the verdict lines the callers search for
        for word in ("VCHECK_SUMMARY", "PASS:", "FAIL:"):
            self.assertNotIn(f'$display("{word}', out)
            self.assertIn(f'$display("RAW_{word}', out)

    def test_unexpected_shape_is_an_error(self) -> None:
        with self.assertRaises(RuntimeError):
            qor_fcheck_run.instrument_testbench(TB.replace("vecMismatched++", "vecBad++"),
                                                OUTPUTS)
        with self.assertRaises(RuntimeError):
            qor_fcheck_run.instrument_testbench(TB.replace("MAX_ERRORS = 20", "MAX_ERRORS = 2 "),
                                                OUTPUTS)

    def test_no_output_ports(self) -> None:
        self.assertIn('$display("VD ");', qor_fcheck_run.instrument_testbench(TB, []))

    def test_x_initial_flag(self) -> None:
        self.assertEqual(qor_fcheck_run._unique_x_initial(["v", "--x-initial", "0", "-O"]),
                         ["v", "--x-initial", "unique", "-O"])
        self.assertEqual(qor_fcheck_run._unique_x_initial(["v"]), ["v", "--x-initial", "unique"])


class CompareRunsTest(unittest.TestCase):
    """Initial-state masking: a bit whose RTL value differs between the runs at any vector is
    not compared at all; every other bit is."""

    def compare(self, run0: list[str], run1: list[str]) -> qor_fcheck_run.InitVerdict:
        unknown = qor_fcheck_run.init_dependent_bits(OUTPUTS, run0, run1)
        return qor_fcheck_run.compare_runs(OUTPUTS, unknown, run0, run1)

    def test_state_dependent_bits_are_masked_not_compared(self) -> None:
        # q: RTL 0b000 (init 0) and 0b111 (init 1): all three bits are x, so the netlist's 0b101
        # (different from both) does not count; done is defined and equal
        v = self.compare([vd((0, 5), (1, 1))], [vd((7, 5), (1, 1))])
        self.assertEqual((v.matched, v.mismatched, v.port_errors), (1, 0, 0))
        self.assertEqual((v.masked_bits, v.compared_bits, v.masked_ports), (3, 1, ["q[3/3]"]))
        self.assertTrue(v.passed)

    def test_a_bit_is_masked_at_every_vector_once_it_depends_on_the_state(self) -> None:
        # the runs agree on q at vectors 0 and 2 and differ in bit 2 at vector 1; the netlist's
        # bit 2 differs from the RTL at vector 0: x for the whole run, not a mismatch at vector 0
        run0 = [vd((2, 6), (0, 0)), vd((2, 6), (0, 0)), vd((2, 6), (0, 0))]
        run1 = [vd((2, 6), (0, 0)), vd((6, 6), (0, 0)), vd((2, 6), (0, 0))]
        v = self.compare(run0, run1)
        self.assertEqual((v.matched, v.mismatched), (3, 0))
        self.assertEqual((v.masked_bits, v.compared_bits), (3, 9))

    def test_mismatch_on_a_defined_bit_is_never_masked(self) -> None:
        # bit 2 depends on the initial state (x); bit 0 is defined 0 in both runs but the netlist
        # drives 1: a mismatch although another bit of the same port is masked
        v = self.compare([vd((0b000, 0b101), (0, 0))], [vd((0b100, 0b101), (0, 0))])
        self.assertEqual((v.matched, v.mismatched, v.port_errors), (0, 1, 1))
        self.assertEqual(v.lines, ["MISMATCH vec=0 port=q rtl=x00 synth=101"])
        self.assertFalse(v.passed)

    def test_mismatch_in_the_second_run_only(self) -> None:
        # the RTL is 0b010 in both runs; a netlist output that moves with the initial state
        # matches run one and not run two: that is a mismatch
        v = self.compare([vd((2, 2), (0, 0))], [vd((2, 3), (0, 0))])
        self.assertEqual((v.matched, v.mismatched), (0, 1))
        self.assertEqual(v.lines, ["MISMATCH vec=0 port=q rtl=010 synth=011"])

    def test_counts_and_vector_numbers(self) -> None:
        run0 = [vd((1, 1), (0, 0)), "tb clocks: clk\n", vd((1, 2), (1, 0)), vd((4, 4), (1, 1))]
        v = self.compare(run0, list(run0))
        self.assertEqual((v.matched, v.mismatched, v.vectors, v.port_errors), (2, 1, 3, 2))
        self.assertEqual([ln.split()[1] for ln in v.lines], ["vec=1", "vec=1"])
        self.assertEqual(v.masked_bits, 0)

    def test_nothing_compared_is_not_a_pass(self) -> None:
        v = self.compare([vd((0, 0), (0, 0))], [vd((7, 0), (1, 0))])  # every bit differs
        self.assertEqual((v.matched, v.compared_bits, v.masked_bits), (1, 0, 4))
        self.assertFalse(v.passed)
        no_ports = qor_fcheck_run.compare_runs([], [], ["VD \n"], ["VD \n"])
        self.assertFalse(no_ports.passed)
        self.assertFalse(self.compare([], []).passed)

    def test_runs_that_do_not_fit_raise(self) -> None:
        with self.assertRaises(ValueError):  # one run dumped fewer vectors
            self.compare([vd((0, 0), (0, 0))], [])
        with self.assertRaises(ValueError):  # a value missing from the dump line
            self.compare(["VD 0:0\n"], ["VD 0:0\n"])

    def test_report_is_capped(self) -> None:
        bad = [vd((0, 1), (0, 0))] * 30
        v = self.compare(bad, bad)
        self.assertEqual((len(v.lines), v.port_errors, v.mismatched), (20, 30, 30))
        self.assertIn("... 10 more port mismatches", qor_fcheck_run.render_verdict(v))

    def test_verdict_text_goes_through_the_callers_verdict_rules(self) -> None:
        same = [vd((1, 1), (1, 1))] * 3
        text = qor_fcheck_run.render_verdict(self.compare(same, same))
        self.assertIn("VCHECK_SUMMARY matched=3 mismatched=0 vectors=3 port_errors=0", text)
        self.assertIn("VCHECK_INIT_MASK masked_bits=0 compared_bits=12 masked_ports=(none)", text)
        self.assertIn("PASS: 3 vectors matched across rtl/post-synth", text)
        self.assertEqual(qor_legs.fcheck_verdict(0, False, text), "pass")
        bad = [vd((1, 0), (1, 1))]
        text = qor_fcheck_run.render_verdict(self.compare(bad, bad))
        self.assertNotIn("PASS:", text)
        self.assertEqual(qor_legs.fcheck_verdict(1, False, text), "fail")
        # every bit masked: no PASS and no mismatch line, an error and never a pass
        text = qor_fcheck_run.render_verdict(
            self.compare([vd((0, 0), (0, 0))], [vd((7, 0), (1, 0))]))
        self.assertNotIn("PASS:", text)
        self.assertEqual(qor_legs.fcheck_verdict(3, False, text), "error")


CHECKER = """\
import subprocess, sys
from pathlib import Path
import blif_to_verilog, tb_generator

class Port:
    def __init__(self, name, direction, width):
        self.name, self.direction, self.width = name, direction, width

work = Path(sys.argv[1])
(work / 'sim_build').mkdir(parents=True)
ports = [Port('clk', 'input', 1), Port('q', 'output', 3), Port('done', 'output', 1)]
tb = tb_generator.generate_dual_testbench(ports)
(work / 'tb.sv').write_text(tb)
with open(work / 'verilator.out', 'w') as log:
    rc = subprocess.call(['verilator', '--binary', '--x-initial', '0'], stdout=log,
                         stderr=subprocess.STDOUT)
sim = work / 'sim_build' / 'Vtb'
sim.write_text(Path(sys.argv[2]).read_text())
sim.chmod(0o755)
with open(work / 'sim.out', 'w') as log:
    rc = subprocess.call([str(sim), '+verilator+seed+1', '+verilator+rand+reset+0'], stdout=log,
                         stderr=subprocess.STDOUT)
print((work / 'sim.out').read_text().strip())
raise SystemExit(rc)
"""


class TwoRunsTest(unittest.TestCase):
    """The wrapper end to end on a stand-in VTR tree: its ``tb_generator``, and a checker that
    compiles with a fake ``verilator`` and runs a fake testbench binary once per initial state."""

    def _run(self, sim_body: str) -> subprocess.CompletedProcess[str]:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as raw:
            tmp = Path(raw)
            vdir = tmp / "verilator_check"
            vdir.mkdir()
            (vdir / "blif_to_verilog.py").write_text(
                "def pack_bit_blasted_top_ports(t):\n    return t\n", encoding="utf-8")
            (vdir / "tb_generator.py").write_text(
                f"def generate_dual_testbench(ports, **kwargs):\n    return {TB!r}\n",
                encoding="utf-8")
            (vdir / "run_random_check.py").write_text(CHECKER, encoding="utf-8")
            bindir = tmp / "bin"
            bindir.mkdir()
            verilator = bindir / "verilator"
            verilator.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
            verilator.chmod(0o755)
            sim = tmp / "sim.sh"
            sim.write_text("#!/bin/sh\necho 'tb clocks: clk'\n" + sim_body, encoding="utf-8")
            env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}")
            run = [sys.executable, "-I", "-B", str(qor_legs.FCHECK_RUNNER),
                   str(vdir / "run_random_check.py"), str(tmp / "work"), str(sim)]
            return subprocess.run(run, capture_output=True, text=True, check=False, env=env)

    def test_pass_when_only_the_initial_state_differs(self) -> None:
        # run 0: RTL q=0 (init 0) vs netlist 5; run 1: RTL q=7 (init 1) vs 5: all x, done fixed
        body = ("case \"$*\" in\n  *reset+0*) echo 'VD 0:5 1:1'; echo 'VD 0:5 1:1';;\n"
                "  *) echo 'VD 7:5 1:1'; echo 'VD 7:5 1:1';;\nesac\necho 'RAW_PASS: 2'\n")
        done = self._run(body)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("VCHECK_INIT_MASK masked_bits=6 compared_bits=2 masked_ports=q[3/3]",
                      done.stdout)
        self.assertIn("VCHECK_SUMMARY matched=2 mismatched=0 vectors=2 port_errors=0", done.stdout)
        self.assertIn("PASS: 2 vectors matched across rtl/post-synth", done.stdout)
        self.assertIn("tb clocks: clk", done.stdout)
        self.assertNotIn("RAW_", done.stdout)
        self.assertEqual(qor_legs.fcheck_verdict(done.returncode, False, done.stdout), "pass")

    def test_a_mismatch_on_a_defined_bit_fails(self) -> None:
        body = "echo 'VD 2:3 1:1'\necho 'RAW_PASS: 1'\n"  # defined in both runs, netlist differs
        done = self._run(body)
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("MISMATCH vec=0 port=q rtl=010 synth=011", done.stdout)
        self.assertNotIn("PASS:", done.stdout)
        self.assertEqual(qor_legs.fcheck_verdict(done.returncode, False, done.stdout), "fail")

    def test_testbench_crash_is_passed_through(self) -> None:
        done = self._run("echo 'VD 0:0 0:0'\nexit 9\n")
        self.assertEqual(done.returncode, 9)
        self.assertNotIn("PASS:", done.stdout)
        self.assertEqual(qor_legs.fcheck_verdict(done.returncode, False, done.stdout), "error")

    def test_missing_dump_in_one_run_is_an_error(self) -> None:
        body = "case \"$*\" in\n  *reset+0*) echo 'VD 0:0 0:0';;\nesac\n"
        done = self._run(body)
        self.assertEqual(done.returncode, 3)
        self.assertIn("ERROR: the two initial-state dumps cannot be compared", done.stdout)
        self.assertEqual(qor_legs.fcheck_verdict(done.returncode, False, done.stdout), "error")

    def test_upstream_without_a_generator_is_reported(self) -> None:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as raw:
            vdir = Path(raw)
            (vdir / "blif_to_verilog.py").write_text(
                "def pack_bit_blasted_top_ports(t):\n    return t\n", encoding="utf-8")
            (vdir / "tb_generator.py").write_text("def renamed(ports):\n    return ''\n",
                                                  encoding="utf-8")
            (vdir / "run_random_check.py").write_text("raise SystemExit(0)\n", encoding="utf-8")
            run = [sys.executable, "-I", "-B", str(qor_legs.FCHECK_RUNNER),
                   str(vdir / "run_random_check.py")]
            done = subprocess.run(run, capture_output=True, text=True, check=False)
            self.assertEqual(done.returncode, 2)
            self.assertIn("has no generate_dual_testbench (renamed or removed upstream)",
                          done.stderr)


if __name__ == "__main__":
    unittest.main()

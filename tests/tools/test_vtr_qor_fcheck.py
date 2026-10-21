"""Tests for the fcheck verdicts and the runner wrapper (3H spec §8.1; ``qor_fcheck_run.py``).

The first real fcheck of the 19 reported ``fail`` for every design in seconds: VTR's
``run_random_check.py`` exits 1 both for a vector mismatch and for a Verilator compile failure,
and the compile failed on a duplicate port declaration its own packing leaves behind.
"""

from __future__ import annotations

import os
import re
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

    def test_untested_needs_its_line_and_no_mismatch(self) -> None:
        line = "UNTESTED: 3 mismatches in 3 vectors, every one on a bit whose RTL value depends\n"
        self.assertEqual(qor_legs.fcheck_verdict(3, False, line), "untested")
        self.assertEqual(qor_legs.fcheck_verdict(0, False, line), "error")  # never a pass
        self.assertEqual(qor_legs.fcheck_verdict(3, False, line + MISMATCH), "fail")
        self.assertEqual(qor_legs.fcheck_verdict(3, False, "no verdict line"), "error")
        self.assertEqual(qor_legs.fcheck_verdict(137, True, line), "capped")

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


LATCH_V = ("module lat(input a, input en, output reg q);\n  always @(a or en) if (en) q = a;\n"
           "endmodule\n")
SENS_V = ("module sens(input a, input b, output reg q);\n  always @(a) q = a & b;\n"
          "endmodule\n")
CLEAN_V = ("module clean(input a, input b, output reg q);\n  always @* q = a & b;\n"
           "endmodule\n")
ODIN_SENS = (
    "odin3: info: read_verilog: 1 units\n"
    "odin3: warning: sens.v:2:1: warning: incomplete sensitivity list: 'b' is read but not "
    "listed; the block is built as @*\n"
    "odin3: warning: sens.v:9:1: warning: incomplete sensitivity list: 'c' is read but not "
    "listed; the block is built as @*\n"
    "odin3: warning: sens.v:11:1: warning: incomplete sensitivity list: 'b' is read but not "
    "listed; the block is built as @*\n")


def _fake_tools(latches: int, odin_out: str):  # type: ignore[no-untyped-def]
    """A ``run`` stand-in: Yosys writes ``<n> objects.`` where ``tee -o`` points, Odin III prints
    ``odin_out``."""
    calls: list[list[str]] = []

    def run(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(list(argv))
        if Path(argv[0]).name == "yosys":
            script = argv[argv.index("-p") + 1]
            target = re.search(r"tee -o (\S+) ", script)
            assert target is not None
            Path(target.group(1)).write_text(f"{latches} objects.\n", encoding="utf-8")
            return subprocess.CompletedProcess(argv, 0, "", "")
        return subprocess.CompletedProcess(argv, 0, odin_out, "")

    return run, calls


class PrecheckTest(unittest.TestCase):
    """H1: an RTL that infers latches (the flow reads it with ``--nolatches``) or has an
    incomplete sensitivity list is ``UNTESTED`` before any simulation."""

    def test_latches_are_a_reason_with_their_count(self) -> None:
        run, calls = _fake_tools(109, "")
        reasons = qor_fcheck_run.rtl_precheck(Path("a.v"), Path("/y/yosys"), None, run)
        self.assertEqual(len(reasons), 1)
        self.assertIn("109 latches", reasons[0])
        self.assertIn("nolatches", reasons[0])
        self.assertEqual(len(calls), 1)  # no Odin III given: only the Yosys read

    def test_sensitivity_signals_are_listed_once_each(self) -> None:
        run, _ = _fake_tools(0, ODIN_SENS)
        reasons = qor_fcheck_run.rtl_precheck(Path("a.v"), Path("/y/yosys"), Path("/o/odin3"), run)
        self.assertEqual(len(reasons), 1)
        self.assertIn("incomplete sensitivity list", reasons[0])
        self.assertIn("b, c", reasons[0])

    def test_the_techlib_is_read_before_the_rtl(self) -> None:
        """Without ``read_techlib`` Odin III cannot elaborate a design with hard blocks and
        reports nothing: the script must load it first."""
        run, calls = _fake_tools(0, "")
        qor_fcheck_run.rtl_precheck(Path("a.v"), None, Path("/o/odin3"), run, Path("/l/vtr.o3lib"))
        self.assertEqual(calls[0][2], "read_techlib /l/vtr.o3lib; read_verilog --vtr-compat a.v")

    def test_both_reasons(self) -> None:
        run, _ = _fake_tools(2, ODIN_SENS)
        reasons = qor_fcheck_run.rtl_precheck(Path("a.v"), Path("/y/yosys"), Path("/o/odin3"), run)
        self.assertEqual(len(reasons), 2)

    def test_a_clean_rtl_has_no_reason(self) -> None:
        run, calls = _fake_tools(0, "odin3: info: read_verilog: 1 units\n")
        self.assertEqual(
            qor_fcheck_run.rtl_precheck(Path("a.v"), Path("/y/yosys"), Path("/o/odin3"), run), [])
        self.assertEqual(len(calls), 2)

    def test_a_failing_tool_is_not_a_verdict(self) -> None:
        """A Yosys that cannot read the RTL says nothing: the check does not hide the design."""
        def run(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(argv, 1, "", "ERROR")
        self.assertEqual(
            qor_fcheck_run.rtl_precheck(Path("a.v"), Path("/y/yosys"), Path("/o/odin3"), run), [])

    def test_timeout_and_oserror_are_no_result_never_a_traceback(self) -> None:
        for exc in (subprocess.TimeoutExpired(["yosys"], 600), OSError("exec format error")):
            def run(argv: list[str], exc: Exception = exc,
                    **kwargs: object) -> subprocess.CompletedProcess[str]:
                raise exc
            notes = qor_fcheck_run.PrecheckNotes()
            reasons = qor_fcheck_run.rtl_precheck(
                Path("a.v"), Path("/y/yosys"), Path("/o/odin3"), run, notes=notes)
            self.assertEqual(reasons, [])
            self.assertEqual(len(notes.skipped), 2, notes.skipped)
            self.assertTrue(notes.skipped[0].startswith("PRECHECK-SKIPPED yosys: "))
            self.assertTrue(notes.skipped[1].startswith("PRECHECK-SKIPPED odin3: "))

    def test_a_failing_tool_is_reported_as_skipped(self) -> None:
        def run(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
            return subprocess.CompletedProcess(argv, 1, "", "ERROR")
        notes = qor_fcheck_run.PrecheckNotes()
        qor_fcheck_run.rtl_precheck(Path("a.v"), Path("/y/yosys"), Path("/o/odin3"), run,
                                    notes=notes)
        self.assertEqual([n.split(":")[0] for n in notes.skipped],
                         ["PRECHECK-SKIPPED yosys", "PRECHECK-SKIPPED odin3"])
        self.assertIn("exit 1", notes.skipped[0])

    def test_a_missing_tool_is_reported_as_skipped(self) -> None:
        run, calls = _fake_tools(0, "")
        notes = qor_fcheck_run.PrecheckNotes()
        qor_fcheck_run.rtl_precheck(Path("a.v"), None, None, run, notes=notes)
        self.assertEqual(calls, [])
        self.assertEqual(len(notes.skipped), 2)
        self.assertTrue(notes.skipped[0].startswith("PRECHECK-SKIPPED yosys: "))
        self.assertTrue(notes.skipped[1].startswith("PRECHECK-SKIPPED odin3: "))

    def test_a_path_with_a_space_is_quoted_for_yosys(self) -> None:
        run, calls = _fake_tools(0, "")
        qor_fcheck_run.rtl_precheck(Path("/t/a b.v"), Path("/y/yosys"), None, run)
        script = calls[0][calls[0].index("-p") + 1]
        self.assertIn('read_verilog "/t/a b.v";', script)

    def test_a_path_that_could_inject_a_command_is_skipped(self) -> None:
        for name in ("/t/a;b.v", '/t/a"b.v', "/t/a\nb.v", "/t/a\\b.v"):
            run, calls = _fake_tools(5, ODIN_SENS)
            notes = qor_fcheck_run.PrecheckNotes()
            reasons = qor_fcheck_run.rtl_precheck(
                Path(name), Path("/y/yosys"), Path("/o/odin3"), run, notes=notes)
            self.assertEqual((reasons, calls), ([], []), name)
            self.assertEqual(len(notes.skipped), 2, name)
            self.assertIn("unsafe", notes.skipped[0])

    def test_odin3_is_skipped_for_a_path_with_a_space(self) -> None:
        run, calls = _fake_tools(0, ODIN_SENS)
        notes = qor_fcheck_run.PrecheckNotes()
        qor_fcheck_run.rtl_precheck(Path("/t/a b.v"), None, Path("/o/odin3"), run, notes=notes)
        self.assertEqual(calls, [])
        self.assertIn("PRECHECK-SKIPPED odin3: ", notes.skipped[-1])

    def test_all_latch_kinds_are_counted_under_the_top(self) -> None:
        run, calls = _fake_tools(0, "")
        qor_fcheck_run.rtl_precheck(Path("a.v"), Path("/y/yosys"), None, run)
        script = calls[0][calls[0].index("-p") + 1]
        self.assertIn("hierarchy -auto-top;", script)
        self.assertIn("select -count t:$dlatch t:$adlatch t:$dlatchsr", script)
        self.assertLess(script.index("hierarchy"), script.index("proc"))
        run, calls = _fake_tools(0, "")
        qor_fcheck_run.rtl_precheck(Path("a.v"), Path("/y/yosys"), None, run, top="core")
        script = calls[0][calls[0].index("-p") + 1]
        self.assertIn("hierarchy -top core;", script)

    def test_short_reasons_for_the_table(self) -> None:
        run, _ = _fake_tools(3, ODIN_SENS)
        notes = qor_fcheck_run.PrecheckNotes()
        qor_fcheck_run.rtl_precheck(Path("a.v"), Path("/y/yosys"), Path("/o/odin3"), run,
                                    notes=notes)
        self.assertEqual(notes.short, ["latches 3", "sensitivity b, c"])
        text = qor_fcheck_run.render_precheck(["x"], notes.short)
        self.assertIn("UNTESTED-REASON latches 3; sensitivity b, c\n", text)
        self.assertEqual(qor_legs.fcheck_verdict(3, False, text), "untested")

    def _wrapper(self, extra_env: dict[str, str], unset: tuple[str, ...] = (),
                 yosys_ok: bool = True) -> subprocess.CompletedProcess[str]:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as tmp:
            vdir = Path(tmp) / "verilator_check"
            vdir.mkdir()
            (vdir / "blif_to_verilog.py").write_text(
                "def pack_bit_blasted_top_ports(text):\n    return text\n", encoding="utf-8")
            (vdir / "run_random_check.py").write_text("print('CHECKER RAN')\n", encoding="utf-8")
            yosys = Path(tmp) / "yosys"  # a Yosys that finds no latch
            yosys.write_text(
                "#!/bin/sh\nout=$(echo \"$3\" | sed 's/.*tee -o \\([^ ]*\\) .*/\\1/')\n"
                "echo '0 objects.' > \"$out\"\n", encoding="utf-8")
            yosys.chmod(0o755)
            rtl = Path(tmp) / "a.v"
            rtl.write_text(CLEAN_V, encoding="utf-8")
            run = [sys.executable, "-I", "-B", str(qor_legs.FCHECK_RUNNER),
                   str(vdir / "run_random_check.py"), "--rtl", str(rtl),
                   "--yosys", str(yosys if yosys_ok else Path(tmp) / "nope")]
            env = {k: v for k, v in os.environ.items() if k not in unset}
            env.update(extra_env)
            return subprocess.run(run, capture_output=True, text=True, check=False, env=env)

    def test_unset_odin3_is_logged_and_the_checker_still_runs(self) -> None:
        done = self._wrapper({}, unset=(qor_fcheck_run.ODIN3_ENV, qor_fcheck_run.TECHLIB_ENV))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("PRECHECK-SKIPPED odin3: ", done.stdout)
        self.assertIn(qor_fcheck_run.ODIN3_ENV, done.stdout)
        self.assertIn("CHECKER RAN", done.stdout)

    def test_missing_yosys_is_logged_and_the_checker_still_runs(self) -> None:
        done = self._wrapper({}, unset=(qor_fcheck_run.ODIN3_ENV,), yosys_ok=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("PRECHECK-SKIPPED yosys: ", done.stdout)
        self.assertIn("CHECKER RAN", done.stdout)

    def test_a_bad_techlib_is_logged_and_the_checker_still_runs(self) -> None:
        done = self._wrapper({qor_fcheck_run.ODIN3_ENV: sys.executable,
                              qor_fcheck_run.TECHLIB_ENV: "/nonexistent/vtr.o3lib"})
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("PRECHECK-SKIPPED odin3: techlib /nonexistent/vtr.o3lib", done.stdout)
        self.assertIn("CHECKER RAN", done.stdout)

    def test_a_skipped_check_does_not_hide_a_reason(self) -> None:
        text = qor_fcheck_run.render_precheck(["RTL infers 3 latches"], ["latches 3"],
                                              ["PRECHECK-SKIPPED odin3: unset"])
        self.assertIn("PRECHECK-SKIPPED odin3: unset\n", text)
        self.assertEqual(qor_legs.fcheck_verdict(3, False, text), "untested")

    def test_real_yosys_an_unused_module_does_not_mark_a_clean_top(self) -> None:
        yosys = (Path.home() / "odin3-ws" / "external" / "vtr-verilog-to-routing" / "build"
                 / "bin" / "yosys")
        if not yosys.is_file():
            self.skipTest("no Yosys build at the controller's usual path")
        text = ("module unused(input d, input en, output reg q);\n"
                "  always @* if (en) q = d;\nendmodule\n"
                "module top(input a, output y);\n  assign y = ~a;\nendmodule\n")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "has space.v"
            path.write_text(text, encoding="utf-8")
            self.assertEqual(qor_fcheck_run.rtl_precheck(path, yosys, None), [])
            reasons = qor_fcheck_run.rtl_precheck(path, yosys, None, top="unused")
        self.assertEqual(len(reasons), 1)

    def test_render_is_an_untested_line_the_caller_reads(self) -> None:
        text = qor_fcheck_run.render_precheck(["RTL infers 3 latches"])
        self.assertEqual(qor_legs.fcheck_verdict(3, False, text), "untested")
        self.assertNotIn("MISMATCH", text)

    def test_wrapper_stops_with_untested_before_the_checker_runs(self) -> None:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as tmp:
            vdir = Path(tmp) / "verilator_check"
            vdir.mkdir()
            (vdir / "blif_to_verilog.py").write_text(
                "def pack_bit_blasted_top_ports(text):\n    return text\n", encoding="utf-8")
            (vdir / "run_random_check.py").write_text(
                "print('CHECKER RAN')\n", encoding="utf-8")
            yosys = Path(tmp) / "yosys"  # a Yosys that finds 4 latches
            yosys.write_text(
                "#!/bin/sh\nout=$(echo \"$3\" | sed 's/.*tee -o \\([^ ]*\\) .*/\\1/')\n"
                "echo '4 objects.' > \"$out\"\n", encoding="utf-8")
            yosys.chmod(0o755)
            rtl = Path(tmp) / "a.v"
            rtl.write_text(LATCH_V, encoding="utf-8")
            run = [sys.executable, "-I", "-B", str(qor_legs.FCHECK_RUNNER),
                   str(vdir / "run_random_check.py"), "--rtl", str(rtl), "--yosys", str(yosys)]
            done = subprocess.run(run, capture_output=True, text=True, check=False)
            self.assertEqual(done.returncode, 3, done.stderr)
            self.assertIn("4 latches", done.stdout)
            self.assertNotIn("CHECKER RAN", done.stdout)
            self.assertEqual(qor_legs.fcheck_verdict(3, False, done.stdout), "untested")

    def _real(self) -> tuple[Path, Path]:
        home = Path.home() / "odin3-ws"
        yosys = home / "external" / "vtr-verilog-to-routing" / "build" / "bin" / "yosys"
        odin3 = home / "work" / "odin3-pmbase" / "build" / "release" / "odin3"
        if not yosys.is_file() or not odin3.is_file():
            self.skipTest("no Yosys / odin3 build at the controller's usual paths")
        return yosys, odin3

    def test_real_tools_latch_sensitivity_and_clean(self) -> None:
        yosys, odin3 = self._real()
        with tempfile.TemporaryDirectory() as tmp:
            got = {}
            for name, text in (("lat", LATCH_V), ("sens", SENS_V), ("clean", CLEAN_V)):
                path = Path(tmp) / f"{name}.v"
                path.write_text(text, encoding="utf-8")
                got[name] = qor_fcheck_run.rtl_precheck(path, yosys, odin3)
        self.assertEqual(len(got["lat"]), 1)
        self.assertIn("1 latches", got["lat"][0])
        self.assertEqual(len(got["sens"]), 1)
        self.assertIn("listed: b)", got["sens"][0])
        self.assertEqual(got["clean"], [])


# The shape of VTR's generated testbench around what the wrapper edits (tb_generator.py).
TB = """\
// no $srandom (verilator pli); seed via +verilator+seed+N / $urandom.
module tb;
    localparam int NUM_VECTORS = 50000;
    localparam int MAX_ERRORS = 20;
    localparam int SEED_HINT = 1;
    initial begin
        for (vecIdx = 0; vecIdx < NUM_VECTORS; vecIdx++) begin
            m = $urandom();
            go = $urandom_range(0, 1);
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

    def test_stimulus_has_a_generator_of_its_own(self) -> None:
        out = qor_fcheck_run.instrument_testbench(TB, OUTPUTS)
        self.assertIsNone(re.search(r"\$urandom\w*\s*\(", out))  # the vectors follow its seed
        with self.assertRaises(RuntimeError):  # a call it does not replace is never left behind
            qor_fcheck_run.instrument_testbench(TB.replace("m = $urandom();", "m = $urandom (3);"),
                                                OUTPUTS)
        self.assertIn("m = rnd32();", out)
        self.assertIn("go = (rnd32() >> 31);", out)
        self.assertEqual(out.count("function automatic [31:0] rnd32();"), 1)
        self.assertLess(out.index("SEED_HINT = 1;"), out.index("rnd_state = rnd_state ^"))
        # the generator's start depends on the seed hint, and is never 0 (xorshift sticks at 0)
        other = qor_fcheck_run.instrument_testbench(TB.replace("SEED_HINT = 1;", "SEED_HINT = 2;"),
                                                    OUTPUTS)
        self.assertNotEqual(out, other)
        self.assertNotIn("32'h00000000;", out)

    def test_unexpected_shape_is_an_error(self) -> None:
        with self.assertRaises(RuntimeError):
            qor_fcheck_run.instrument_testbench(TB.replace("vecMismatched++", "vecBad++"),
                                                OUTPUTS)
        with self.assertRaises(RuntimeError):
            qor_fcheck_run.instrument_testbench(TB.replace("MAX_ERRORS = 20", "MAX_ERRORS = 2 "),
                                                OUTPUTS)
        with self.assertRaises(RuntimeError):  # no seed hint to start the generator from
            qor_fcheck_run.instrument_testbench(TB.replace("SEED_HINT", "SEED_HUNT"), OUTPUTS)

    def test_no_output_ports(self) -> None:
        self.assertIn('$display("VD ");', qor_fcheck_run.instrument_testbench(TB, []))

    def test_x_initial_flag(self) -> None:
        self.assertEqual(qor_fcheck_run._unique_x_initial(["v", "--x-initial", "0", "-O"]),
                         ["v", "--x-initial", "unique", "-O"])
        self.assertEqual(qor_fcheck_run._unique_x_initial(["v"]), ["v", "--x-initial", "unique"])


class CompareRunsTest(unittest.TestCase):
    """The first run (all uninitialised state 0, the netlist's own start) decides; the other runs
    only say whether a mismatching bit is ``x``."""

    def compare(self, *runs: list[str]) -> qor_fcheck_run.InitVerdict:
        return qor_fcheck_run.compare_runs(OUTPUTS, runs)

    def test_a_clean_first_run_is_a_pass_with_no_other_run(self) -> None:
        run = [vd((5, 5), (1, 1)), vd((2, 2), (0, 0))]
        v = self.compare(run)
        self.assertEqual((v.matched, v.mismatched, v.x_dependent, v.vectors), (2, 0, 0, 2))
        self.assertEqual(v.compared_bits, 8)
        self.assertTrue(v.passed)

    def test_mismatch_without_other_runs_is_real(self) -> None:
        v = self.compare([vd((5, 4), (1, 1))])
        self.assertEqual((v.matched, v.mismatched, v.port_errors), (0, 1, 1))
        self.assertEqual(v.lines, ["MISMATCH vec=0 port=q rtl=101 synth=100"])
        self.assertFalse(v.passed)

    def test_mismatch_on_a_state_dependent_bit_is_x_not_a_pass(self) -> None:
        # run 0: q=0b000, netlist 0b101; run 1: q=0b111; run 2: 0b010: bits 2, 1, 0 all differ
        # between the runs, so every mismatched bit is x: reported, but never a pass
        v = self.compare([vd((0, 5), (1, 1))], [vd((7, 5), (1, 1))], [vd((2, 5), (1, 1))])
        self.assertEqual((v.matched, v.mismatched, v.x_dependent), (0, 0, 1))
        self.assertEqual((v.port_errors, v.x_port_errors), (0, 1))
        self.assertEqual(v.x_lines, ["XDEPEND vec=0 port=q rtl=xxx synth=101"])
        self.assertFalse(v.passed)

    def test_a_defined_bit_that_mismatches_makes_a_fail_whatever_else_is_x(self) -> None:
        # bit 2 depends on the initial state; bit 0 is 0 in every run but the netlist drives 1
        v = self.compare([vd((0b000, 0b101), (0, 0))], [vd((0b100, 0b101), (0, 0))],
                         [vd((0b000, 0b101), (0, 0))])
        self.assertEqual((v.mismatched, v.x_dependent, v.port_errors), (1, 0, 1))
        self.assertEqual(v.lines, ["MISMATCH vec=0 port=q rtl=x00 synth=101"])

    def test_only_the_vector_of_the_dependence_is_x(self) -> None:
        # the runs agree on q at vector 0 (a defined mismatch) and differ at vector 1
        run0 = [vd((0, 1), (0, 0)), vd((0, 1), (0, 0))]
        run1 = [vd((0, 1), (0, 0)), vd((1, 1), (0, 0))]
        v = self.compare(run0, run1)
        self.assertEqual((v.mismatched, v.x_dependent, v.matched), (1, 1, 0))
        self.assertEqual(v.lines, ["MISMATCH vec=0 port=q rtl=000 synth=001"])
        self.assertEqual(v.x_lines, ["XDEPEND vec=1 port=q rtl=00x synth=001"])

    def test_a_third_initial_state_can_expose_what_two_miss(self) -> None:
        # bit 1 of q is the same under init 0 and init 1 and differs under the random init
        v = self.compare([vd((0, 2), (0, 0))], [vd((0, 2), (0, 0))], [vd((2, 2), (0, 0))])
        self.assertEqual((v.mismatched, v.x_dependent), (0, 1))
        self.assertEqual(v.x_lines, ["XDEPEND vec=0 port=q rtl=0x0 synth=010"])

    def test_rtl_x_and_netlist_varies_is_xdepend(self) -> None:
        # H2: the RTL bit changes with the start: XDEPEND, whatever the netlist does
        v = self.compare([vd((0, 1), (0, 0))], [vd((1, 0), (0, 0))], [vd((0, 1), (0, 0))])
        self.assertEqual((v.matched, v.mismatched, v.x_dependent), (0, 0, 1))
        self.assertEqual(v.x_lines, ["XDEPEND vec=0 port=q rtl=00x synth=00x"])
        self.assertEqual(v.init_depend_lines, [])
        self.assertFalse(v.passed)

    def test_rtl_stable_and_netlist_varies_is_netlist_init_depend(self) -> None:
        # RTL stable at 0 in every start, the netlist bit 0 is 1 in run 0 and 0 in run 1
        v = self.compare([vd((0, 1), (0, 0))], [vd((0, 0), (0, 0))], [vd((0, 1), (0, 0))])
        self.assertEqual((v.matched, v.mismatched, v.x_dependent), (0, 0, 1))
        self.assertEqual(v.x_lines, [])
        self.assertEqual(v.init_depend_lines, ["NETLIST_INIT_DEPEND vec=0 port=q rtl=000 "
                                               "synth=00x"])
        self.assertEqual(v.init_depend_port_errors, 1)
        self.assertFalse(v.passed)
        text = qor_fcheck_run.render_verdict(v)
        self.assertIn("NETLIST_INIT_DEPEND vec=0 port=q", text)
        self.assertIn("UNTESTED-REASON NETLIST_INIT_DEPEND\n", text)
        self.assertNotIn("MISMATCH", text)
        self.assertNotIn("XDEPEND vec", text.replace("NETLIST_INIT_DEPEND vec", ""))
        self.assertEqual(qor_legs.fcheck_verdict(3, False, text), "untested")

    def test_x_and_netlist_init_depend_in_one_run_are_both_named(self) -> None:
        # q bit 1: RTL varies; q bit 0: RTL stable 0, netlist varies
        v = self.compare([vd((0b00, 0b11), (0, 0))], [vd((0b10, 0b10), (0, 0))])
        self.assertEqual(v.x_lines, ["XDEPEND vec=0 port=q rtl=0x0 synth=01x"])
        self.assertEqual(v.init_depend_lines,
                         ["NETLIST_INIT_DEPEND vec=0 port=q rtl=0x0 synth=01x"])
        text = qor_fcheck_run.render_verdict(v)
        self.assertIn("UNTESTED-REASON XDEPEND, NETLIST_INIT_DEPEND\n", text)

    def test_stable_in_both_is_mismatch(self) -> None:
        v = self.compare([vd((0, 1), (0, 0))], [vd((0, 1), (0, 0))], [vd((0, 1), (0, 0))])
        self.assertEqual((v.mismatched, v.x_dependent), (1, 0))
        self.assertEqual(v.init_depend_lines, [])
        self.assertEqual(v.lines, ["MISMATCH vec=0 port=q rtl=000 synth=001"])

    def test_netlist_bit_stable_across_inits_is_still_a_mismatch(self) -> None:
        # the netlist's bit 1 varies (x) but bit 0 is a stable 1 against a stable RTL 0
        v = self.compare([vd((0, 0b011), (0, 0))], [vd((0, 0b001), (0, 0))])
        self.assertEqual((v.mismatched, v.x_dependent), (1, 0))
        self.assertEqual(v.lines, ["MISMATCH vec=0 port=q rtl=000 synth=0x1"])

    def test_netlist_variation_makes_only_its_own_vector_x(self) -> None:
        # no vector is skipped: vector 0 varies, vector 1 does not and mismatches
        run0 = [vd((0, 1), (0, 0)), vd((0, 1), (0, 0))]
        run1 = [vd((0, 0), (0, 0)), vd((0, 1), (0, 0))]
        v = self.compare(run0, run1)
        self.assertEqual((v.mismatched, v.x_dependent, v.vectors), (1, 1, 2))

    def test_vector_numbers_and_other_lines(self) -> None:
        run = [vd((1, 1), (0, 0)), "tb clocks: clk\n", vd((1, 2), (1, 0)), vd((4, 4), (1, 1))]
        v = self.compare(run)
        self.assertEqual((v.matched, v.mismatched, v.vectors, v.port_errors), (2, 1, 3, 2))
        self.assertEqual([ln.split()[1] for ln in v.lines], ["vec=1", "vec=1"])

    def test_nothing_compared_is_not_a_pass(self) -> None:
        self.assertFalse(qor_fcheck_run.compare_runs([], [["VD \n"]]).passed)
        self.assertFalse(self.compare([]).passed)

    def test_runs_that_do_not_fit_raise(self) -> None:
        with self.assertRaises(ValueError):  # one run dumped fewer vectors
            self.compare([vd((0, 0), (0, 0))], [])
        with self.assertRaises(ValueError):  # a value missing from the dump line
            self.compare(["VD 0:0\n"])

    def test_report_is_capped(self) -> None:
        bad = [vd((0, 1), (0, 0))] * 30
        v = self.compare(bad)
        self.assertEqual((len(v.lines), v.port_errors, v.mismatched), (20, 30, 30))
        self.assertIn("... 10 more MISMATCH ports", qor_fcheck_run.render_verdict(v))

    def test_verdict_text_goes_through_the_callers_verdict_rules(self) -> None:
        same = [vd((1, 1), (1, 1))] * 3
        text = qor_fcheck_run.render_verdict(self.compare(same))
        self.assertIn("VCHECK_SUMMARY matched=3 mismatched=0 vectors=3 port_errors=0", text)
        self.assertIn("PASS: 3 vectors matched across rtl/post-synth", text)
        self.assertEqual(qor_legs.fcheck_verdict(0, False, text), "pass")
        bad = [vd((1, 0), (1, 1))]
        text = qor_fcheck_run.render_verdict(self.compare(bad, bad, bad))
        self.assertNotIn("PASS:", text)
        self.assertIn("FAIL: 1 mismatches in 1 vectors", text)
        self.assertEqual(qor_legs.fcheck_verdict(1, False, text), "fail")
        # only x mismatches: no PASS, no MISMATCH line, a summary with mismatched=0 and exit 3:
        # the callers' ``error``, never a pass and never a fail
        text = qor_fcheck_run.render_verdict(
            self.compare([vd((0, 5), (1, 1))], [vd((7, 5), (1, 1))], [vd((2, 5), (1, 1))]))
        self.assertNotIn("PASS:", text)
        self.assertNotIn("MISMATCH", text)
        self.assertIn("UNTESTED: 1 mismatches in 1 vectors", text)
        self.assertIn("x_dependent_vectors=1", text)
        self.assertEqual(qor_legs.fcheck_verdict(3, False, text), "untested")
        self.assertEqual(qor_legs.fcheck_verdict(0, False, text), "error")  # exit 0 is no pass


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


class InitRunsTest(unittest.TestCase):
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

    def test_clean_first_run_passes_and_stops(self) -> None:
        # the first run is clean: no further run (a second invocation would print the marker)
        body = ("case \"$*\" in\n  *reset+0*) echo 'VD 2:2 1:1'; echo 'VD 5:5 0:0';;\n"
                "  *) echo 'RAN-AGAIN';;\nesac\necho 'RAW_PASS: 2'\n")
        done = self._run(body)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("VCHECK_SUMMARY matched=2 mismatched=0 vectors=2 port_errors=0", done.stdout)
        self.assertIn("PASS: 2 vectors matched across rtl/post-synth", done.stdout)
        self.assertIn("tb clocks: clk", done.stdout)
        self.assertNotIn("RAW_", done.stdout)
        self.assertEqual(qor_legs.fcheck_verdict(done.returncode, False, done.stdout), "pass")

    def test_mismatch_on_state_dependent_bits_is_untested(self) -> None:
        # run 0: RTL q=0 (init 0) vs netlist 5; run 1: q=7; run 2: q=2: every bit is x
        body = ("case \"$*\" in\n  *reset+0*) echo 'VD 0:5 1:1';;\n"
                "  *reset+1*) echo 'VD 7:5 1:1';;\n  *) echo 'VD 2:5 1:1';;\nesac\n")
        done = self._run(body)
        self.assertEqual(done.returncode, 3, done.stderr)
        self.assertIn("XDEPEND vec=0 port=q rtl=xxx synth=101", done.stdout)
        self.assertIn("UNTESTED: 1 mismatches in 1 vectors", done.stdout)
        self.assertNotIn("PASS:", done.stdout)
        self.assertNotIn("MISMATCH", done.stdout)
        self.assertEqual(qor_legs.fcheck_verdict(done.returncode, False, done.stdout), "untested")

    def test_a_mismatch_on_a_defined_bit_fails(self) -> None:
        body = "echo 'VD 2:3 1:1'\n"  # the same in every run, the netlist differs
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

    def test_runs_of_different_length_are_an_error(self) -> None:
        # run 0 has a mismatch (so the others run); run 1 dumps nothing
        body = "case \"$*\" in\n  *reset+0*) echo 'VD 0:1 0:0';;\nesac\n"
        done = self._run(body)
        self.assertEqual(done.returncode, 3)
        self.assertIn("ERROR: the initial-state dumps cannot be compared", done.stdout)
        self.assertEqual(qor_legs.fcheck_verdict(done.returncode, False, done.stdout), "error")

    def test_other_runs_change_the_initial_state_and_the_seed(self) -> None:
        import io

        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as raw:
            build = Path(raw) / "sim_build"
            build.mkdir()
            calls: list[list[str]] = []

            def fake_call(argv: list[str], stdout: io.TextIOBase, stderr: int) -> int:
                calls.append(argv)
                stdout.write("VD 2:3 1:1\n")  # the netlist differs, the RTL never depends on state
                return 0

            out = io.StringIO()
            code = qor_fcheck_run._run_inits(
                [str(build / "Vtb"), "+verilator+seed+7", "+verilator+rand+reset+0"], OUTPUTS, out,
                fake_call)
            self.assertEqual(code, 1)
            self.assertEqual([(c[1], c[-1]) for c in calls],
                             [("+verilator+seed+7", "+verilator+rand+reset+0"),
                              ("+verilator+seed+7", "+verilator+rand+reset+1")]
                             + [(f"+verilator+seed+{7 + n}", "+verilator+rand+reset+2")
                                for n in (1, 2, 3, 4)])
            self.assertEqual(sorted(p.name for p in Path(raw).iterdir()), ["sim_build"])
            self.assertIn("FAIL: 1 mismatches in 1 vectors", out.getvalue())

    def _classify(self, rows_by_mode: dict[str, list[str]]) -> tuple[int, str, list[list[str]]]:
        """``_run_inits`` over a fake testbench whose dump depends on ``+rand+reset+N``."""
        import io

        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as raw:
            build = Path(raw) / "sim_build"
            build.mkdir()
            calls: list[list[str]] = []

            def fake_call(argv: list[str], stdout: io.TextIOBase, stderr: int) -> int:
                calls.append(argv)
                stdout.write("tb clocks: clk\n" + "".join(rows_by_mode[argv[-1][-1]]))
                return 0

            out = io.StringIO()
            code = qor_fcheck_run._run_inits(
                [str(build / "Vtb"), "+verilator+seed+1", "+verilator+rand+reset+0"], OUTPUTS,
                out, fake_call)
            self.assertEqual(sorted(p.name for p in Path(raw).iterdir()), ["sim_build"])
            return code, out.getvalue(), calls

    def test_netlist_init_one_against_zero_rtl_varying_across_runs_is_untested(self) -> None:
        # a netlist register with BLIF init 1 starts at 1, the zero-init RTL at 0: run 0 shows a
        # mismatch; the RTL value changes with the other starts, so it is x: never a pass, no fail
        rows = {"0": [vd((0, 1), (0, 0))], "1": [vd((1, 1), (0, 0))], "2": [vd((1, 1), (0, 0))]}
        code, text, calls = self._classify(rows)
        self.assertEqual((code, len(calls)), (3, 6))
        self.assertIn("XDEPEND vec=0 port=q rtl=00x synth=001", text)
        self.assertIn("UNTESTED:", text)
        self.assertNotIn("PASS:", text)
        self.assertEqual(qor_legs.fcheck_verdict(code, False, text), "untested")

    def test_netlist_init_one_against_stable_zero_rtl_fails(self) -> None:
        rows = {m: [vd((0, 1), (0, 0))] for m in "012"}  # RTL 0 in every start, netlist 1
        code, text, _ = self._classify(rows)
        self.assertEqual(code, 1)
        self.assertIn("MISMATCH vec=0 port=q rtl=000 synth=001", text)
        self.assertEqual(qor_legs.fcheck_verdict(code, False, text), "fail")

    def test_one_vector_both_kinds_and_clean_vectors_are_counted(self) -> None:
        # vector 1: bit 0 differs (stable RTL), bit 2 differs and is x; vectors 0, 2 are clean
        clean = vd((3, 3), (0, 0))
        rows = {"0": [clean, vd((0, 5), (0, 0)), clean],
                "1": [clean, vd((4, 5), (0, 0)), clean],
                "2": [clean, vd((0, 5), (0, 0)), clean]}
        code, text, _ = self._classify(rows)
        self.assertEqual(code, 1)
        self.assertIn("MISMATCH vec=1 port=q rtl=x00 synth=101", text)
        self.assertIn("VCHECK_SUMMARY matched=2 mismatched=1 vectors=3 port_errors=1", text)

    def test_keep_vectors_filters_a_dump(self) -> None:
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as raw:
            path = Path(raw) / "d.out"
            path.write_text("hdr\n" + vd((1, 1), (0, 0)) + vd((2, 2), (0, 0)) + vd((3, 3), (0, 0)))
            qor_fcheck_run.keep_vectors(path, {1})
            self.assertEqual(path.read_text(), vd((2, 2), (0, 0)))
            run = [vd((1, 2), (0, 0)), vd((1, 1), (0, 0)), vd((2, 2), (0, 0))]
            self.assertEqual(qor_fcheck_run.mismatching_vectors(OUTPUTS, run), (3, [0]))

    def test_dumps_are_removed_after_the_verdict(self) -> None:
        import io

        with tempfile.TemporaryDirectory(dir=HEAVY_DIR) as raw:
            build = Path(raw) / "sim_build"
            build.mkdir()
            calls: list[list[str]] = []

            def fake_call(argv: list[str], stdout: io.TextIOBase, stderr: int) -> int:
                calls.append(argv)
                stdout.write("tb clocks: clk\nVD 2:2 1:1\n")
                return 0

            out = io.StringIO()
            code = qor_fcheck_run._run_inits([str(build / "Vtb"), "+verilator+seed+1"], OUTPUTS,
                                             out, fake_call)
            self.assertEqual(code, 0)
            self.assertEqual(len(calls), 1)  # a clean first run needs no other initial state
            self.assertTrue(calls[0][-1].endswith("rand+reset+0"))
            self.assertEqual(sorted(p.name for p in Path(raw).iterdir()), ["sim_build"])
            self.assertIn("PASS: 1 vectors matched", out.getvalue())

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

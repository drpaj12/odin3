"""Tests for tools/exit-check (2H; PHASE2 #4, decision 13), over fake sp-splice, netlist-compare
and equiv-check scripts and a fake golden tree."""

from __future__ import annotations

import contextlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Sequence
from pathlib import Path
from unittest import mock

import elab_sweep
import exit_check
import memcap

from . import helpers

MEMINFO_32G = "MemTotal:       33554432 kB\nMemFree:        1000 kB\n"
GIB = 1 << 30

# The fake sp-splice copies the design's source to <keep>/out.blif; markers in the source steer it.
FAKE_SPLICE = """#!{python}
import json, os, signal, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["sp-splice", *argv]) + "\\n")
text = open(argv[-1]).read()
if "SKIP77" in text:
    sys.exit(77)
if "SPLICEKILL" in text:
    print("sp-splice: error: Yosys (the Yosys half) failed (exit -9):\\nKilled", file=sys.stderr)
    sys.exit(2)
if "SPLICESEGV" in text:
    print("sp-splice: error: odin3 (the Odin III half) failed (exit -11):\\nboom", file=sys.stderr)
    sys.exit(2)
if "SPLICEFAIL" in text:
    print("sp-splice: error: odin3 (the Odin III half) failed (exit 1):\\n"
          "odin3: error: 'a' has two drivers", file=sys.stderr)
    sys.exit(2)
keep = argv[argv.index("--keep") + 1]
out = os.path.join(keep, "out.blif")
with open(out, "w") as handle:
    handle.write(text)
print(out)
"""

FAKE_COMPARE = """#!{python}
import json, os, signal, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["netlist-compare", *argv]) + "\\n")
text = open(argv[1]).read()
if "NCKILL" in text:
    os.kill(os.getpid(), signal.SIGKILL)
if "NCERR" in text:
    print("netlist-compare: cannot order tied cells", file=sys.stderr)
    sys.exit(2)
if "IDENT" in text:
    sys.exit(0)
print("--- a\\n+++ b\\n-.names x\\n+.names y")
sys.exit(1)
"""

FAKE_EQUIV = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["equiv-check", *argv]) + "\\n")
text = open(argv[-1]).read()
if "EQERR" in text:
    print("equiv-check: unsupported: more than one clock domain", file=sys.stderr)
    sys.exit(2)
if "EQV" in text:
    print("equiv-check: equivalent (abc cec)")
    sys.exit(0)
if "XCONE" in text:
    print("equiv-check: NOT equivalent (abc cec)")
    print("equiv-check: x-report: 2 of 9 outputs are in an x-cone (unconn reaches them)")
    print("equiv-check: equivalent outside x-cones (abc cec)")
    print("x-cone: o[3]\\nx-cone: o[4]")
    sys.exit(3)
print("equiv-check: NOT equivalent (abc cec)")
print("equiv-check: NOT equivalent outside x-cones (abc cec)")
sys.exit(1)
"""


def _exe(path: Path, text: str) -> Path:
    path.write_text(text.replace("{python}", sys.executable), encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class ExitCheckCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        bin_dir = self.dir / "bin"
        bin_dir.mkdir()
        self.splice = _exe(bin_dir / "sp-splice", FAKE_SPLICE)
        self.compare = _exe(bin_dir / "netlist-compare", FAKE_COMPARE)
        self.equiv = _exe(bin_dir / "equiv-check", FAKE_EQUIV)
        self.odin3 = _exe(bin_dir / "odin3", "#!{python}\n")
        self.yosys = _exe(bin_dir / "yosys", "#!{python}\n")
        self.arch = self.dir / "arch.xml"
        self.arch.write_text("<architecture/>\n", encoding="utf-8")
        self.log = self.dir / "argv.log"
        self.meminfo = self.dir / "meminfo"
        self.meminfo.write_text(MEMINFO_32G, encoding="ascii")
        self.golden = self.dir / "golden"
        self.micros = self.dir / "tests" / "micro" / "verilog"
        self.work = self.dir / "work"
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log)}))
        stack.enter_context(mock.patch.object(memcap, "MEMINFO", str(self.meminfo)))
        stack.enter_context(mock.patch.object(memcap, "SYSTEMD_RUN", "no-such-systemd-run"))
        self.addCleanup(stack.close)

    def micro(self, group: str, leaf: str, text: str, top: str | None = "top",
              golden_text: str | None = None) -> Path:
        """A micro source and (unless ``top`` is None) its Parmys golden."""
        path = self.micros / group / f"{leaf}.v"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        if top is not None:
            blif = self.golden / "regression" / "verilog" / group / leaf / f"{leaf}.parmys.blif"
            blif.parent.mkdir(parents=True, exist_ok=True)
            blif.write_text(golden_text or f".model {top}\n.end\n", encoding="utf-8")
        return path

    def run_tool(self, *args: str, inputs: Sequence[Path] | None = None) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--arch", str(self.arch),
                "--sp-splice", str(self.splice), "--netlist-compare", str(self.compare),
                "--equiv-check", str(self.equiv), "--golden-root", str(self.golden),
                "--work", str(self.work), "--root", str(self.micros),
                *([] if "--vtr-config-template" in args else ["--no-config"]), *args,
                *map(str, inputs if inputs is not None else [self.micros])]
        return helpers.run_main(exit_check.main, argv)

    def calls(self, tool: str | None = None) -> list[list[str]]:
        if not self.log.is_file():
            return []
        rows = [json.loads(line) for line in self.log.read_text().splitlines()]
        return [row for row in rows if tool is None or row[0] == tool]

    def expect(self, text: str) -> str:
        path = self.dir / "expect.txt"
        path.write_text(text, encoding="utf-8")
        return str(path)


class ClassesTest(ExitCheckCase):
    def test_each_class_and_the_exit_code(self) -> None:
        self.micro("g", "a_ident", "IDENT\n")
        self.micro("g", "b_equiv", "EQV\n")
        self.micro("g", "c_xcone", "XCONE\n")
        self.micro("g", "d_differs", "nothing special\n")
        self.micro("g", "e_error", "SPLICEFAIL\n")
        code, out, err = self.run_tool()
        self.assertEqual(code, 1, out + err)
        self.assertIn("identical  g/a_ident.v", out)
        self.assertIn("equivalent g/b_equiv.v", out)
        self.assertIn("eq-x       g/c_xcone.v", out)
        self.assertIn("2 output(s) in x-cones: o[3], o[4]", out)
        self.assertIn("differs    g/d_differs.v", out)
        self.assertIn("NOT equivalent outside x-cones (abc cec)", out)
        self.assertIn("error      g/e_error.v", out)
        self.assertIn("sp-splice: 'a' has two drivers", out)
        self.assertIn("5 designs: 1 identical, 1 equivalent, 1 equivalent outside x-cones, "
                      "1 differs, 1 error, 0 listed", out)

    def test_identical_and_equivalent_exit_0(self) -> None:
        self.micro("g", "a", "IDENT\n")
        self.micro("g", "b", "EQV\n")
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        self.assertIn("2 designs: 1 identical, 1 equivalent", out)

    def test_an_unlisted_eq_x_does_not_count(self) -> None:
        self.micro("g", "x", "XCONE\n")
        self.assertEqual(self.run_tool()[0], 1)

    def test_a_listed_eq_x_counts_with_its_outputs_recorded(self) -> None:
        self.micro("g", "x", "XCONE\n")
        listing = self.expect("g/x.v [x-cone: o[3]] o[3] and o[4] reviewed: read an x memory bit\n")
        code, out, err = self.run_tool("--expect-fail", listing)
        self.assertEqual(code, 0, out + err)
        self.assertIn("listed     g/x.v", out)
        self.assertIn("1 listed", out)

    def test_smallest_first(self) -> None:
        self.micro("g", "big", "IDENT" + " " * 300 + "\n")
        self.micro("g", "tiny", "IDENT\n")
        self.micro("h", "mid", "IDENT" + " " * 50 + "\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 0)
        order = [line.split()[1] for line in out.splitlines() if line.startswith("identical")]
        self.assertEqual(order, ["g/tiny.v", "h/mid.v", "g/big.v"])
        spliced = [Path(c[-1]).name for c in self.calls("sp-splice")]
        self.assertEqual(spliced, ["tiny.v", "mid.v", "big.v"])

    def test_netlist_compare_error_falls_through_to_equiv_check(self) -> None:
        self.micro("g", "tie", "NCERR EQV\n")
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        self.assertIn("equivalent g/tie.v", out)
        log = (self.work / "0000-g_tie.v" / "exit-check.log").read_text()
        self.assertIn("netlist-compare exit 2: not a verdict", log)

    def test_equiv_check_error_is_an_error(self) -> None:
        self.micro("g", "multi", "EQERR\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("error      g/multi.v", out)
        self.assertIn("equiv-check: unsupported: more than one clock domain", out)


class ArgvTest(ExitCheckCase):
    def test_children_get_the_golden_top_arch_and_cap(self) -> None:
        self.micro("g", "a", "IDENT\n", top="my_top")
        self.micro("g", "b", "EQV\n", top="b_top")
        self.assertEqual(self.run_tool("--mem-cap", "3G")[0], 0)
        a = next(c for c in self.calls("sp-splice") if c[-1].endswith("/a.v"))
        self.assertEqual(a[a.index("--top") + 1], "my_top")
        self.assertEqual(a[a.index("--arch") + 1], str(self.arch))
        self.assertEqual(a[a.index("--mem-cap") + 1], str(3 * GIB))
        self.assertEqual(a[a.index("--odin3") + 1], str(self.odin3))
        self.assertEqual(a[a.index("--yosys") + 1], str(self.yosys))
        self.assertTrue(a[a.index("--keep") + 1].startswith(str(self.work)))
        compare = next(c for c in self.calls("netlist-compare") if "/a/" in c[1])
        self.assertTrue(compare[1].endswith("g/a/a.parmys.blif"))
        self.assertTrue(compare[2].endswith("out.blif"))
        equiv = self.calls("equiv-check")[0]  # only the design netlist-compare did not settle
        self.assertEqual(equiv[1:3], ["--x-report", "--match-dollar"])
        self.assertTrue(equiv[-2].endswith("g/b/b.parmys.blif"))
        self.assertEqual(len(self.calls("equiv-check")), 1)

    def test_the_comparison_children_run_through_memcap(self) -> None:
        self.micro("g", "a", "EQV\n")
        with mock.patch.object(memcap, "run", wraps=memcap.run) as run:
            self.assertEqual(self.run_tool("--mem-cap", "2G")[0], 0)
        capped = {Path(call.args[0][0]).name: call.args[1] for call in run.call_args_list}
        self.assertEqual(capped["netlist-compare"], 2 * GIB)
        self.assertEqual(capped["equiv-check"], 2 * GIB)

    def test_optional_splice_arguments_are_passed_on(self) -> None:
        self.micro("g", "a", "IDENT\n")
        self.assertEqual(self.run_tool("--techlib", "T.o3lib", "--primitives", "P.v",
                                       "--config", "C.xml")[0], 0)
        a = self.calls("sp-splice")[0]
        self.assertEqual(a[a.index("--techlib") + 1], "T.o3lib")
        self.assertEqual(a[a.index("--primitives") + 1], "P.v")
        self.assertEqual(a[a.index("--config") + 1], "C.xml")

    def test_vtr_config_is_filled_per_design_as_vtr_fills_it(self) -> None:
        self.arch.write_text(
            "<architecture><pb_type blif_model=\".subckt single_port_ram\">"
            "<input name=\"addr\" num_pins=\"15\"/><input name=\"data\" num_pins=\"64\"/>"
            "</pb_type><pb_type blif_model=\".subckt dual_port_ram\">"
            "<input name=\"addr1\" num_pins=\"14\"/></pb_type></architecture>\n",
            encoding="utf-8")
        template = self.dir / "template.xml"
        template.write_text(
            "<config><in>TTT XXX</in><arch>YYY</arch><mult size=\"MMM\" fixed=\"1\"/>"
            "<mem depth=\"PPP\"/><add threshold=\"AAA\"/><out>ZZZ</out></config>\n",
            encoding="utf-8")
        src = self.micro("g", "a", "IDENT\n")
        code, out, err = self.run_tool("--vtr-config-template", str(template))
        self.assertEqual(code, 0, out + err)
        call = self.calls("sp-splice")[0]
        config = Path(call[call.index("--config") + 1])
        self.assertEqual(config.parent, self.work / "0000-g_a.v")
        text = config.read_text()
        for expected in (f"Verilog {src}", "<arch>arch.xml</arch>", 'size="3" fixed="1"',
                         'depth="15"', 'threshold="1"'):
            self.assertIn(expected, text)
        code, out, err = self.run_tool("--vtr-config-template", str(template),
                                       "--min-hard-mult-size", "5", "--min-hard-adder-size", "0")
        self.assertIn('size="5"', config.read_text())
        self.assertIn('threshold="0"', config.read_text())

    def test_no_template_is_a_usage_error(self) -> None:
        self.micro("g", "a", "IDENT\n")
        with mock.patch.dict(os.environ, {"VTR_ROOT": str(self.dir / "none")}):
            code, _, err = helpers.run_main(exit_check.main, [
                "--odin3", str(self.odin3), "--yosys", str(self.yosys), "--arch", str(self.arch),
                str(self.micros)])
        self.assertEqual(code, 2)
        self.assertIn("basic_odin_config_split.xml not found", err)

    def test_a_cap_for_design_gets_its_cap_and_runs_alone(self) -> None:
        self.micro("g", "small", "IDENT\n")
        self.micro("g", "big", "IDENT" + " " * 99 + "\n")
        code, out, err = self.run_tool("--mem-cap", "1G", "--cap-for", "g/big.v=5G",
                                       "--jobs", "2")
        self.assertEqual(code, 0, out + err)
        caps = {Path(c[-1]).name: c[c.index("--mem-cap") + 1] for c in self.calls("sp-splice")}
        self.assertEqual(caps, {"small.v": str(GIB), "big.v": str(5 * GIB)})

    def test_jobs_times_cap_above_sixty_percent_is_refused(self) -> None:
        self.micro("g", "a", "IDENT\n")
        code, _, err = self.run_tool("--jobs", "8", "--mem-cap", "4G")
        self.assertEqual(code, 2)
        self.assertIn("exceeds 60% of MemTotal", err)
        self.assertEqual(self.calls(), [])

    def test_vtr_benchmarks_find_their_golden_under_vtr(self) -> None:
        bench = self.dir / "vtr_flow" / "benchmarks" / "verilog"
        (bench / "koios").mkdir(parents=True)
        (bench / "ch_intrinsics.v").write_text("IDENT\n", encoding="utf-8")
        (bench / "koios" / "gemm.v").write_text("EQV " + "x" * 20 + "\n", encoding="utf-8")
        for rel, top in (("ch_intrinsics/ch_intrinsics", "memset"), ("koios/gemm/gemm", "gemm")):
            blif = self.golden / "vtr" / f"{rel}.parmys.blif"
            blif.parent.mkdir(parents=True)
            blif.write_text(f".model {top}\n.end\n", encoding="utf-8")
        code, out, err = self.run_tool(inputs=[bench])
        self.assertEqual(code, 0, out + err)
        tops = {Path(c[-1]).name: c[c.index("--top") + 1] for c in self.calls("sp-splice")}
        self.assertEqual(tops, {"ch_intrinsics.v": "memset", "gemm.v": "gemm"})

    def test_limit_keeps_the_smallest_designs_with_a_golden(self) -> None:
        self.micro("g", "nogold", "x\n", top=None)
        self.micro("g", "a", "IDENT" + " " * 10 + "\n")
        self.micro("g", "b", "IDENT" + " " * 20 + "\n")
        self.micro("g", "c", "IDENT" + " " * 30 + "\n")
        code, out, err = self.run_tool("--limit", "2")
        self.assertEqual(code, 0, out + err)
        self.assertIn("2 designs: 2 identical", out)
        self.assertNotIn("nogold", out)
        self.assertNotIn("g/c.v", out)


class ListedTest(ExitCheckCase):
    def test_no_golden_is_an_error_and_may_be_listed(self) -> None:
        self.micro("g", "rej", "x\n", top=None)
        prov = self.golden / "regression" / "verilog" / "g" / "rej" / "rej.parmys.prov"
        prov.parent.mkdir(parents=True)
        prov.write_text("tool=parmys\nstatus=failed\n", encoding="utf-8")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("error      g/rej.v", out)
        self.assertIn("Parmys rejected the design (status=failed)", out)
        self.assertEqual(self.calls(), [])
        listing = self.expect("g/rej.v [Parmys rejected the design] Parmys rejects it: x\n")
        code, out, _ = self.run_tool("--expect-fail", listing)
        self.assertEqual(code, 0, out)
        self.assertIn("): error: Parmys rejects it: x", out)

    def test_a_listed_design_that_passes_is_xpass(self) -> None:
        self.micro("g", "a", "IDENT\n")
        code, out, _ = self.run_tool("--expect-fail", self.expect("g/a.v because\n"))
        self.assertEqual(code, 1)
        self.assertIn("XPASS      g/a.v", out)
        self.assertIn("1 listed but passing", out)

    def test_a_wrong_needle_fails(self) -> None:
        self.micro("g", "d", "nothing\n")
        code, out, _ = self.run_tool("--expect-fail", self.expect("g/d.v [no such text] x\n"))
        self.assertEqual(code, 1)
        self.assertIn("fails differently from the listing: expected [no such text]", out)
        code, out, _ = self.run_tool("--expect-fail",
                                     self.expect("g/d.v [NOT equivalent outside] proven: x\n"))
        self.assertEqual(code, 0, out)

    def test_a_signal_or_cap_kill_is_never_expected(self) -> None:
        self.micro("g", "oom", "SPLICEKILL\n")
        self.micro("g", "segv", "SPLICESEGV\n")
        self.micro("g", "nc", "NCKILL\n")
        listing = self.expect("g/oom.v [Yosys] x\ng/segv.v [odin3] x\ng/nc.v [exit] x\n")
        code, out, _ = self.run_tool("--expect-fail", listing, "--mem-cap", "3G")
        self.assertEqual(code, 1)
        self.assertIn("out of memory (cap 3G)", out)
        self.assertEqual(out.count("is never an expected result"), 3)
        self.assertIn("listed, hard failure", out)

    def test_a_stale_entry_fails_unless_partial(self) -> None:
        self.micro("g", "a", "IDENT\n")
        listing = self.expect("g/gone.v [x] was removed\n")
        code, out, _ = self.run_tool("--expect-fail", listing)
        self.assertEqual(code, 1)
        self.assertIn("STALE g/gone.v", out)
        self.assertEqual(self.run_tool("--expect-fail", listing, "--partial")[0], 0)

    def test_the_list_is_elab_sweeps_parser(self) -> None:
        self.micro("g", "a", "IDENT\n")
        listing = self.expect("g/a.v no reason is fine\ng/a.v twice\n")
        with mock.patch.object(elab_sweep, "merge_expect", wraps=elab_sweep.merge_expect) as m:
            code, _, err = self.run_tool("--expect-fail", listing)
        self.assertEqual(code, 2)
        self.assertIn("'g/a.v' is listed twice", err)
        m.assert_called_once()

    def test_a_skipped_design_is_not_a_failure_alone(self) -> None:
        self.micro("g", "a", "SKIP77\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 77)
        self.assertIn("skip", out)


class LogTest(ExitCheckCase):
    def test_a_log_per_design_and_a_summary_are_written_and_kept(self) -> None:
        self.micro("g", "a", "IDENT\n")
        self.micro("g", "b", "nothing special\n")
        self.assertEqual(self.run_tool()[0], 1)
        log = (self.work / "0000-g_a.v" / "exit-check.log").read_text()
        self.assertIn("design g/a.v", log)
        self.assertIn("top top", log)
        self.assertIn("result identical", log)
        bad = (self.work / "0001-g_b.v" / "exit-check.log").read_text()
        self.assertIn("NOT equivalent outside x-cones", bad)
        self.assertIn("result differs", bad)
        self.assertTrue((self.work / "0000-g_a.v" / "out.blif").is_file())
        summary = (self.work / "summary.txt").read_text()
        self.assertIn("2 designs:", summary)

    def test_help_and_usage_errors(self) -> None:
        code, out, _ = helpers.run_main(exit_check.main, ["--help"])
        self.assertEqual(code, 0)
        self.assertIn("--expect-fail", out)
        code, _, err = helpers.run_main(exit_check.main, [
            "--odin3", str(self.odin3), "--yosys", "no-such-yosys", "--arch", str(self.arch),
            str(self.micros)])
        self.assertEqual(code, 2)
        self.assertIn("no-such-yosys is not an executable file", err)

    def test_the_wrapper_script_runs(self) -> None:
        wrapper = helpers.REPO_ROOT / "tools" / "exit-check" / "exit-check"
        self.assertTrue(os.access(wrapper, os.X_OK))
        done = subprocess.run([str(wrapper), "--help"], capture_output=True, text=True,
                              check=False)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("exit-check", done.stdout)


if __name__ == "__main__":
    unittest.main()

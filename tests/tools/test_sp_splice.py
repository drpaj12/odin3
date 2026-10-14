"""Tests for tools/opt-check/sp-splice and phase2.script (2F plan Task 15, spec section 3, OP-3),
over fake odin3 and yosys scripts that log their argv; memcap runs the children directly (no
systemd-run on PATH)."""

from __future__ import annotations

import contextlib
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import memcap
import sp_splice

from . import helpers

ORACLE = "Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0 -fPIC -O3)"
MEMINFO_32G = "MemTotal:       33554432 kB\nMemFree:        1000 kB\n"
PHASE2_SCRIPT = helpers.REPO_ROOT / "tools" / "opt-check" / "phase2.script"

ODIN3_SCRIPT = (
    "read_techlib /lib/vtr.o3lib; read_verilog --lib --nomem2reg /vtr/prim.v; "
    "read_verilog --nolatches --vtr-compat /src/a.v /src/b.v; "
    "hierarchy --top top; opt_expr; opt_clean; opt -nodffe -nosdff; proc; write_rtlil a.il"
)
YOSYS_SCRIPT = (
    "plugin -i parmys; yosys -import; parmys_arch -a /arch/k6.xml; "
    "read_verilog -nomem2reg /vtr/prim.v; "
    "setattr -mod -set keep_hierarchy 1 single_port_ram; "
    "setattr -mod -set keep_hierarchy 1 dual_port_ram; "
    "read_rtlil -nooverwrite a.il; "
    "hierarchy -check -auto-top -purge_lib; procs -norom; "
    "fsm; opt; wreduce; peepopt; opt_clean; share; opt -full; memory -nomap; flatten; opt -full; "
    "techmap -map +/parmys/adff2dff.v; techmap -map +/parmys/adffe2dff.v; "
    "techmap -map +/parmys/aldff2dff.v; techmap -map +/parmys/aldffe2dff.v; opt -full; "
    "parmys -a /arch/k6.xml -nopass -c /cfg/c.xml; opt -full; techmap; opt -fast; "
    "dffunmap; opt -fast -noff; stat; hierarchy -check -auto-top -purge_lib; "
    "write_blif -true + vcc -false + gnd -undef + unconn -blackbox out.blif"
)
SCC_SCRIPT = (
    "read_verilog -nomem2reg /vtr/prim.v; read_verilog -sv -nolatches /src/a.v /src/b.v; "
    "scc -select; select -assert-none %"
)

# The fake odin3 logs its argv and writes a.il when the script says write_rtlil a.il.
FAKE_ODIN3 = """#!{python}
import json, os, sys
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["odin3"] + sys.argv[1:]) + "\\n")
# Like the real CLI: read_verilog elaborates at once, so several modules need --top on the command
# line (hierarchy --top in the script comes too late).
words = [w.strip('"') for w in sys.argv[-1].replace(";", " ").split()]
modules = sum(open(w).read().count("\\nmodule ") + open(w).read().startswith("module ")
              for w in words if w.endswith(".v") and os.path.isfile(w))
if modules > 1 and "--top" not in sys.argv[1:-2]:
    print("odin3: error: ambiguous_top", file=sys.stderr)
    sys.exit(2)
if os.environ.get("FAKE_ODIN3_RC", "0") != "0":
    print("odin3: error: boom", file=sys.stderr)
    sys.exit(int(os.environ["FAKE_ODIN3_RC"]))
if "write_rtlil a.il" in sys.argv[-1]:
    open("a.il", "w").write("module \\\\t\\nend\\n")
"""

# The fake yosys (-p SCRIPT, or -c FILE: the file's text is logged last) logs its argv, answers
# -V, fails a script that holds FAKE_YOSYS_FAIL_ON, and
# writes out.blif for the script that holds write_blif.
FAKE_YOSYS = """#!{python}
import json, os, sys
script = open(sys.argv[2]).read() if sys.argv[1:2] == ["-c"] else sys.argv[-1]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["yosys"] + sys.argv[1:] + ([script] if sys.argv[1:2] == ["-c"] else []))
              + "\\n")
if sys.argv[1:] == ["-V"]:
    print(os.environ.get("FAKE_YOSYS_VERSION", "{oracle}"))
    sys.exit(0)
bad = os.environ.get("FAKE_YOSYS_FAIL_ON")
if bad and bad in script:
    print("ERROR: yosys boom", file=sys.stderr)
    sys.exit(1)
if "out.blif" in script and os.environ.get("FAKE_NO_BLIF") is None:
    open("out.blif", "w").write(".model top\\n.end\\n")
"""


def _exe(path: Path, text: str) -> Path:
    path.write_text(text.format(python=sys.executable, oracle=ORACLE), encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class SpSpliceTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        self.odin3 = _exe(self.bin / "odin3", FAKE_ODIN3)
        self.yosys = _exe(self.bin / "yosys", FAKE_YOSYS)
        self.a = self.dir / "a.v"
        self.b = self.dir / "b.v"
        for src in (self.a, self.b):
            src.write_text("module t; endmodule\n", encoding="utf-8")
        self.keep = self.dir / "keep"
        self.log = self.dir / "argv.log"
        meminfo = self.dir / "meminfo"
        meminfo.write_text(MEMINFO_32G, encoding="ascii")
        env = {"FAKE_LOG": str(self.log), "PATH": str(self.bin)}
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.dict(os.environ, env))
        stack.enter_context(mock.patch.object(memcap, "MEMINFO", str(meminfo)))
        self.addCleanup(stack.close)

    def run_tool(self, *args: str, extra: dict[str, str] | None = None,
                 files: list[Path] | None = None) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--arch", "/arch/k6.xml",
                "--top", "top", "--primitives", "/vtr/prim.v", "--techlib", "/lib/vtr.o3lib",
                "--keep", str(self.keep), *args,
                *map(str, files or [self.a, self.b])]
        with mock.patch.dict(os.environ, extra or {}):
            return helpers.run_main(sp_splice.main, argv)

    def calls(self) -> list[list[str]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def scripts(self) -> list[tuple[str, str]]:
        """(tool, -p script) of every child that ran a script."""
        return [(c[0], c[-1]) for c in self.calls() if "-p" in c or "-c" in c]

    def expected_odin3(self) -> str:
        return ODIN3_SCRIPT.replace("/src/a.v", str(self.a)).replace("/src/b.v", str(self.b))

    # --- the two halves ---------------------------------------------------------------------

    def test_both_command_strings_exactly(self) -> None:
        code, out, err = self.run_tool("--config", "/cfg/c.xml")
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(self.scripts(), [("odin3", self.expected_odin3()),
                                          ("yosys", "yosys -import\n" + YOSYS_SCRIPT + "\n")])
        self.assertEqual(self.calls()[-1][:3], ["yosys", "-c", "yosys.tcl"])
        self.assertEqual(out.strip(), str(self.keep / "out.blif"))
        self.assertTrue((self.keep / "a.il").is_file())

    def test_odin3_half_spellings(self) -> None:
        self.run_tool()
        script = self.scripts()[0][1]
        self.assertIn("read_verilog --lib --nomem2reg /vtr/prim.v;", script)
        self.assertIn("read_verilog --nolatches --vtr-compat ", script)
        self.assertIn("hierarchy --top top;", script)
        self.assertNotIn("-sv", script)

    def test_top_is_a_cli_option_before_p(self) -> None:
        """Review I1: a file with several uninstantiated modules needs `odin3 --top T -p ...`."""
        multi = self.dir / "multi.v"
        multi.write_text("module a; endmodule\nmodule b; endmodule\n", encoding="utf-8")
        code, _, err = self.run_tool("--no-primitives", "--primitives", "", files=[multi])
        self.assertEqual((code, err), (0, ""))
        odin3 = next(c for c in self.calls() if c[0] == "odin3")
        self.assertEqual(odin3[:4], ["odin3", "--top", "top", "-p"])
        self.assertIn("hierarchy --top top;", odin3[4])

    def test_yosys_half_matches_vtr_state_at_hierarchy(self) -> None:
        """2H: synthesis.tcl's steps 1 to 4 in order: the primitives, keep_hierarchy on the two
        RAMs, our design (a.il; a stub of the same name does not replace the real module), then
        hierarchy; the library modules' processes are lowered as VTR's `procs -norom` does."""
        self.run_tool()
        yosys = self.scripts()[1][1].split("; ")
        start = yosys.index("read_verilog -nomem2reg /vtr/prim.v")
        self.assertEqual(yosys[start:start + 6], [
            "read_verilog -nomem2reg /vtr/prim.v",
            "setattr -mod -set keep_hierarchy 1 single_port_ram",
            "setattr -mod -set keep_hierarchy 1 dual_port_ram",
            "read_rtlil -nooverwrite a.il",
            "hierarchy -check -auto-top -purge_lib",
            "procs -norom"])

    def test_default_techlib_is_the_repo_one(self) -> None:
        self.assertEqual(sp_splice.DEFAULT_TECHLIB, helpers.REPO_ROOT / "lib" / "vtr.o3lib")
        self.assertTrue(sp_splice.DEFAULT_TECHLIB.is_file())

    def test_top_stays_on_the_odin3_side(self) -> None:
        self.run_tool()
        odin3, yosys = (script for _, script in self.scripts())
        self.assertIn("hierarchy --top top", odin3)
        self.assertNotIn("hierarchy --top", yosys)
        self.assertNotIn("hierarchy -top", yosys)
        self.assertIn("hierarchy -check -auto-top -purge_lib", yosys)

    def test_pre_commands_and_no_primitives(self) -> None:
        code, _, _ = self.run_tool("--odin3-pre", "read_techlib /lib/other.o3lib", "--no-techlib",
                                   "--no-primitives", "--primitives", "")
        self.assertEqual(code, 0)
        expected = ODIN3_SCRIPT.replace("/src/a.v", str(self.a)).replace("/src/b.v", str(self.b))
        expected = expected.replace("read_techlib /lib/vtr.o3lib; ", "")
        expected = expected.replace("read_verilog --lib --nomem2reg /vtr/prim.v; ", "")
        self.assertEqual(self.scripts()[0][1], "read_techlib /lib/other.o3lib; " + expected)
        # The Yosys half still reads VTR's own spelling of the primitives (synthesis.tcl step 1).
        self.assertIn("read_verilog -nomem2reg +/parmys/vtr_primitives.v; ", self.scripts()[1][1])

    def test_no_config_leaves_parmys_without_c(self) -> None:
        self.run_tool()
        self.assertIn("parmys -a /arch/k6.xml -nopass; opt -full", self.scripts()[1][1])

    def test_odin3_half_is_phase2_script_up_to_proc(self) -> None:
        commands = phase2_commands()
        script = self.expected_odin3().replace("/vtr/prim.v", "P")
        halves = script.split("; ")
        self.assertEqual(halves[4:9], commands[:4] + ["write_rtlil a.il"])

    def test_scc_precheck_runs_first_in_yosys_spelling(self) -> None:
        code, _, _ = self.run_tool("--scc-precheck")
        scc = SCC_SCRIPT.replace("/src/a.v", str(self.a)).replace("/src/b.v", str(self.b))
        self.assertEqual(code, 0)
        self.assertEqual([tool for tool, _ in self.scripts()], ["yosys", "odin3", "yosys"])
        self.assertEqual(self.scripts()[0][1], scc)
        self.assertIn("-nolatches", scc)

    def test_scc_precheck_failure_is_exit_2_and_stops(self) -> None:
        code, _, err = self.run_tool("--scc-precheck", extra={"FAKE_YOSYS_FAIL_ON": "scc -select"})
        self.assertEqual(code, 2)
        self.assertIn("scc precondition", err)
        self.assertEqual([tool for tool, _ in self.scripts()], ["yosys"])

    def test_quotes_a_file_name_with_a_blank(self) -> None:
        spaced = self.dir / "has blank.v"
        spaced.write_text("module t; endmodule\n", encoding="utf-8")
        self.run_tool(files=[spaced])
        self.assertIn(f'read_verilog --nolatches --vtr-compat "{spaced}";', self.scripts()[0][1])

    # --- exit mapping -----------------------------------------------------------------------

    def test_odin3_failure_is_exit_2_and_yosys_never_runs(self) -> None:
        code, out, err = self.run_tool(extra={"FAKE_ODIN3_RC": "1"})
        self.assertEqual((code, out), (2, ""))
        self.assertIn("odin3 (the Odin III half) failed (exit 1)", err)
        self.assertIn("boom", err)
        self.assertEqual([tool for tool, _ in self.scripts()], ["odin3"])

    def test_yosys_failure_is_exit_2(self) -> None:
        code, out, err = self.run_tool(extra={"FAKE_YOSYS_FAIL_ON": "write_blif"})
        self.assertEqual((code, out), (2, ""))
        self.assertIn("Yosys (the Yosys half) failed (exit 1)", err)

    def test_missing_out_blif_is_exit_2(self) -> None:
        code, _, err = self.run_tool(extra={"FAKE_NO_BLIF": "1"})
        self.assertEqual(code, 2)
        self.assertIn("wrote no out.blif", err)

    def test_missing_tool_is_exit_2(self) -> None:
        argv = ["--odin3", str(self.dir / "nope"), "--yosys", str(self.yosys), "--arch", "a",
                "--top", "t", "--primitives", "p.v", "--keep", str(self.keep), str(self.a)]
        code, _, err = helpers.run_main(sp_splice.main, argv)
        self.assertEqual(code, 2)
        self.assertIn("cannot run", err)

    def test_usage_error_is_exit_2(self) -> None:
        code, _, _ = helpers.run_main(sp_splice.main, ["--odin3", "o"])
        self.assertEqual(code, 2)

    # --- the Yosys version guard ------------------------------------------------------------

    def test_refuses_yosys_0_69(self) -> None:
        code, out, err = self.run_tool(extra={"FAKE_YOSYS_VERSION": "Yosys 0.69 (git sha1 abc)"})
        self.assertEqual((code, out), (2, ""))
        self.assertIn("not Yosys 0.55", err)
        self.assertEqual(self.scripts(), [])

    def test_allow_version_accepts_a_named_drift_version(self) -> None:
        code, _, _ = self.run_tool("--allow-version", "0.69",
                                   extra={"FAKE_YOSYS_VERSION": "Yosys 0.69 (git sha1 abc)"})
        self.assertEqual(code, 0)

    def test_version_ok_is_not_fooled_by_0_550_or_0_5(self) -> None:
        for text in ("Yosys 0.550", "Yosys 0.5", "Yosys 10.55"):
            self.assertFalse(sp_splice.version_ok(text, None), text)
        self.assertTrue(sp_splice.version_ok(ORACLE, None))

    # --- every child goes through memcap.run ------------------------------------------------

    def test_every_child_goes_through_memcap(self) -> None:
        real = memcap.run
        seen: list[tuple[str, int]] = []

        def spy(argv: Any, cap: int, *args: Any, **kwargs: Any) -> Any:
            seen.append((Path(argv[0]).name, cap))
            return real(argv, cap, *args, **kwargs)

        with mock.patch.object(memcap, "run", spy):
            code, _, _ = self.run_tool("--mem-cap", str(1 << 30))
        self.assertEqual(code, 0)
        self.assertEqual(seen, [("yosys", 1 << 30), ("odin3", 1 << 30), ("yosys", 1 << 30)])


def phase2_commands() -> list[str]:
    """phase2.script's commands: '#' comments and blank lines dropped, ';' and newline split."""
    commands: list[str] = []
    for line in PHASE2_SCRIPT.read_text(encoding="utf-8").splitlines():
        for part in line.split("#", 1)[0].split(";"):
            if part.strip():
                commands.append(part.strip())
    return commands


class Phase2ScriptTest(unittest.TestCase):
    def test_phase2_script_is_opt_proc_opt_compact(self) -> None:
        # IR-6 unchanged (PHASE2 #19): opt never compacts, the script does. proc ends with
        # opt_expr -keepdc (step F, wired), so no separate opt_expr follows it.
        self.assertEqual(phase2_commands(),
                         ["opt_expr", "opt_clean", "opt -nodffe -nosdff", "proc", "opt", "compact"])

    def test_bench_opt_runs_the_same_commands(self) -> None:
        bench = (helpers.REPO_ROOT / "tests" / "bench" / "bench_opt.c").read_text(encoding="utf-8")
        for command in ("opt_expr", "opt_clean", "opt -nodffe -nosdff", "proc", "opt", "compact"):
            self.assertIn(f'"{command}', bench)


if __name__ == "__main__":
    unittest.main()

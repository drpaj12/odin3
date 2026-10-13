"""Tests for tools/elab-check/elab-equiv and memcap.py (2D Task 21; spec §14.3, plan point (n)),
over fake odin3, yosys, equiv-check and systemd-run scripts that log their argv."""

from __future__ import annotations

import contextlib
import json
import os
import resource
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import elab_equiv
import memcap

from . import helpers

ORACLE = "Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0 -fPIC -O3)"
GIB = 1 << 30
MEMINFO_32G = "MemTotal:       33554432 kB\nMemFree:        1000 kB\n"

# The fake odin3 logs its argv and cwd and writes the file of every `write_rtlil NAME` it is told.
FAKE_ODIN3 = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["odin3", *argv, os.getcwd()]) + "\\n")
rc = int(os.environ.get("FAKE_ODIN3_RC", "0"))
if rc:
    print("odin3: error: no", file=sys.stderr)
    sys.exit(rc)
for cmd in (c.strip() for c in argv[argv.index("-p") + 1].split(";")):
    if cmd.startswith("write_rtlil "):
        with open(cmd.split()[1], "w") as out:
            out.write("module \\\\t\\nend\\n")
"""

# The fake Yosys prints a version for -V; for -p it writes the file of every write_blif and
# write_rtlil, and exits $FAKE_YOSYS_RC when the script holds $FAKE_YOSYS_FAIL.
FAKE_YOSYS = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["yosys", *argv, os.getcwd()]) + "\\n")
if argv == ["-V"]:
    print(os.environ.get("FAKE_YOSYS_VERSION", "{oracle}"))
    sys.exit(0)
script = argv[argv.index("-p") + 1]
sig = os.environ.get("FAKE_YOSYS_SIGNAL")
if sig and os.environ.get("FAKE_YOSYS_SIGNAL_ON", "") in script:
    os.kill(os.getpid(), int(sig))
fail = os.environ.get("FAKE_YOSYS_FAIL")
if fail and fail in script:
    print("ERROR: no")
    sys.exit(1)
for cmd in (c.strip() for c in script.split(";")):
    if cmd.startswith("write_blif "):
        with open(cmd.split()[-1], "w") as out:
            out.write(".model t\\n.end\\n")
    elif cmd.startswith("write_rtlil "):
        with open(cmd.split()[1], "w") as out:
            out.write("module \\\\t\\nend\\n")
"""

FAKE_EQUIV = """#!{python}
import json, os, sys
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["equiv-check", *sys.argv[1:], os.getcwd()]) + "\\n")
rc = os.environ.get("FAKE_EQUIV_RC", "0")
if rc == "1" and not os.environ.get("FAKE_EQUIV_NO_VERDICT"):
    print("equiv-check: NOT equivalent (abc cec)")
else:
    print("equiv-check says", rc)
sys.exit(int(rc))
"""

# systemd-run: logs, drops its own options, then runs the command.
FAKE_SYSTEMD_RUN = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_SYSTEMD_LOG"], "a") as log:
    log.write(json.dumps(["systemd-run", *argv]) + "\\n")
i = 0
while i < len(argv) and argv[i].startswith("-"):
    i += 2 if argv[i] == "-p" else 1
os.execv(argv[i], argv[i:])
"""

FAKE_PRLIMIT = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_SYSTEMD_LOG"], "a") as log:
    log.write(json.dumps(["prlimit", *argv]) + "\\n")
os.execv(argv[1], argv[1:])
"""


def _exe(path: Path, text: str) -> Path:
    path.write_text(text.replace("{python}", sys.executable).replace("{oracle}", ORACLE),
                    encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class BridgeTest(unittest.TestCase):
    """The bridge over fakes; memcap runs the children directly (no systemd-run on PATH)."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        self.odin3 = _exe(self.bin / "odin3", FAKE_ODIN3)
        self.yosys = _exe(self.bin / "yosys", FAKE_YOSYS)
        self.equiv = _exe(self.bin / "equiv-check", FAKE_EQUIV)
        self.src = self.dir / "t.v"
        self.src.write_text("module t; endmodule\n", encoding="utf-8")
        self.keep = self.dir / "keep"
        self.log = self.dir / "argv.log"
        meminfo = self.dir / "meminfo"
        meminfo.write_text(MEMINFO_32G, encoding="ascii")
        env = {"FAKE_LOG": str(self.log), "PATH": str(self.bin)}
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.dict(os.environ, env))
        stack.enter_context(mock.patch.object(memcap, "MEMINFO", str(meminfo)))
        self.addCleanup(stack.close)

    def run_tool(self, *args: str, extra: dict[str, str] | None = None) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--equiv-check",
                str(self.equiv), "--keep", str(self.keep), *args, str(self.src)]
        with mock.patch.dict(os.environ, extra or {}):
            return helpers.run_main(elab_equiv.main, argv)

    def calls(self) -> list[list[str]]:
        if not self.log.exists():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def names(self) -> list[str]:
        return [call[0] for call in self.calls()]

    # -- the command strings ---------------------------------------------------------------

    BACK = ("proc; flatten; opt; memory -nomap; opt; memory_map; opt -full; techmap; opt -fast; "
            "abc -g AND,NAND,OR,NOR,XOR,XNOR,MUX; opt_clean; write_blif -gates -undef + unconn ")

    def test_commands_exactly_with_top(self) -> None:
        code, out, err = self.run_tool("--top", "t")
        self.assertEqual(code, 0, out + err)
        work = str(self.keep.resolve())
        f = str(self.src)
        self.assertEqual(self.calls(), [
            ["yosys", "-V", work],
            ["odin3", "--top", "t", "-p", f"read_verilog --nolatches {f}; write_rtlil a.il", work],
            ["yosys", "-p", "read_rtlil a.il; hierarchy -top t; " + self.BACK + "a.blif", work],
            ["yosys", "-p", f"read_verilog -sv -nolatches {f}; hierarchy -top t; " + self.BACK
             + "b.blif", work],
            ["yosys", "-p", "read_rtlil a.il; write_rtlil b.il", work],
            ["equiv-check", "a.blif", "b.blif", work],
        ])
        self.assertIn("t.v: equivalent", out)

    def test_commands_without_top(self) -> None:
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        f = str(self.src)
        work = str(self.keep.resolve())
        calls = self.calls()
        script = f"read_verilog --nolatches {f}; write_rtlil a.il"
        self.assertEqual(calls[1], ["odin3", "-p", script, work])
        self.assertEqual(calls[2][2], "read_rtlil a.il; hierarchy -auto-top; " + self.BACK
                         + "a.blif")
        self.assertEqual(calls[3][2], f"read_verilog -sv -nolatches {f}; hierarchy -auto-top; "
                         + self.BACK + "b.blif")

    # -- --pass proc (2E spec 18.5, point (r)) ----------------------------------------------

    TRIO = "opt_expr; opt_clean; opt -nodffe -nosdff; "

    def test_pass_proc_side_a_is_exactly_the_phase2_script_up_to_proc(self) -> None:
        code, out, err = self.run_tool("--pass", "proc", "--top", "t")
        self.assertEqual(code, 0, out + err)
        work = str(self.keep.resolve())
        f = str(self.src)
        self.assertEqual(self.calls(), [
            ["yosys", "-V", work],
            ["odin3", "--top", "t", "-p",
             f"read_verilog --nolatches {f}; " + self.TRIO + "proc; write_rtlil a.il", work],
            ["yosys", "-p", "read_rtlil a.il; hierarchy -top t; " + self.BACK + "a.blif", work],
            ["yosys", "-p", f"read_verilog -sv -nolatches {f}; hierarchy -top t; " + self.BACK
             + "b.blif", work],
            ["yosys", "-p", "read_rtlil a.il; write_rtlil b.il", work],
            ["equiv-check", "a.blif", "b.blif", work],
        ])

    def test_pass_proc_no_trio_drops_the_three_passes(self) -> None:
        code, out, err = self.run_tool("--pass", "proc", "--no-trio")
        self.assertEqual(code, 0, out + err)
        f = str(self.src)
        self.assertEqual(self.calls()[1][2],
                         f"read_verilog --nolatches {f}; proc; write_rtlil a.il")

    def test_pass_elab_is_the_default_and_ignores_the_trio(self) -> None:
        self.run_tool("--no-trio")
        f = str(self.src)
        self.assertEqual(self.calls()[1][2], f"read_verilog --nolatches {f}; write_rtlil a.il")
        self.run_tool("--pass", "elab")
        self.assertEqual(self.calls()[-5][2], f"read_verilog --nolatches {f}; write_rtlil a.il")

    def test_pass_proc_side_b_and_back_end_unchanged(self) -> None:
        self.run_tool("--pass", "proc", "--unmap-enables", "--top", "t")
        f = str(self.src)
        calls = self.calls()
        self.assertEqual(calls[3][2], elab_equiv.yosys_script(
            f"read_verilog -sv -nolatches {f}", "t", "b.blif", unmap=True))
        self.assertEqual(calls[2][2], elab_equiv.yosys_script(
            "read_rtlil a.il", "t", "a.blif", unmap=True))

    def test_pass_other_than_elab_or_proc_is_a_usage_error(self) -> None:
        code, _, err = self.run_tool("--pass", "opt")
        self.assertEqual(code, 2)
        self.assertIn("invalid choice", err)
        self.assertEqual(self.calls(), [])

    def test_nolatches_on_both_sides(self) -> None:
        self.run_tool()
        scripts = [c[2] for c in self.calls() if c[0] in ("odin3", "yosys") and "-p" in c]
        self.assertTrue(all("-nolatches" in s or "read_rtlil" in s for s in scripts))
        self.assertEqual(sum("--nolatches" in s or "-sv -nolatches" in s for s in scripts), 2)

    def test_files_are_kept(self) -> None:
        self.run_tool("--top", "t")
        for name in ("a.il", "a.blif", "b.blif", "b.il"):
            self.assertTrue((self.keep / name).is_file(), name)

    def test_without_keep_the_directory_is_removed(self) -> None:
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--equiv-check",
                str(self.equiv), str(self.src)]
        code, _, _ = helpers.run_main(elab_equiv.main, argv)
        self.assertEqual(code, 0)
        work = Path(self.calls()[0][-1])
        self.assertFalse(work.exists())

    # -- exit codes ------------------------------------------------------------------------

    def test_exit_code_mapping(self) -> None:
        for rc, want in ((0, 0), (1, 1), (2, 2), (3, 2)):
            self.log.unlink(missing_ok=True)
            code, out, err = self.run_tool(extra={"FAKE_EQUIV_RC": str(rc)})
            self.assertEqual(code, want, out + err)
        self.assertIn("NOT equivalent", self.run_tool(extra={"FAKE_EQUIV_RC": "1"})[1])

    def test_exit_one_without_the_verdict_line_is_an_error(self) -> None:
        """A crash of equiv-check (a traceback exits 1) is not "NOT equivalent" (review I5)."""
        code, out, err = self.run_tool(extra={"FAKE_EQUIV_RC": "1", "FAKE_EQUIV_NO_VERDICT": "1"})
        self.assertEqual(code, 2, out + err)
        self.assertNotIn("NOT equivalent", out)
        self.assertIn("equiv-check failed (exit 1, no verdict line)", err)

    def test_a_tool_that_is_not_an_executable_is_an_error(self) -> None:
        """A wrong --equiv-check/--odin3/--yosys path exits 2 before any child runs (review I5)."""
        for flag in ("--odin3", "--yosys", "--equiv-check"):
            self.log.unlink(missing_ok=True)
            argv = {"--odin3": str(self.odin3), "--yosys": str(self.yosys),
                    "--equiv-check": str(self.equiv)}
            argv[flag] = str(self.dir / "nonexistent")
            args = [x for pair in argv.items() for x in pair]
            code, out, err = helpers.run_main(elab_equiv.main, [*args, str(self.src)])
            self.assertEqual(code, 2, flag + out + err)
            self.assertIn(f"{flag} {self.dir / 'nonexistent'} is not an executable", err)
            self.assertEqual(self.names(), [], flag)

    def test_a_only_does_not_need_equiv_check(self) -> None:
        code, _, err = helpers.run_main(
            elab_equiv.main,
            ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--equiv-check",
             str(self.dir / "nonexistent"), "--keep", str(self.keep), "--a-only", str(self.src)])
        self.assertEqual(code, 0, err)

    def test_odin3_failure_is_an_error(self) -> None:
        code, _, err = self.run_tool(extra={"FAKE_ODIN3_RC": "1"})
        self.assertEqual(code, 2)
        self.assertIn("odin3 (side A) failed (exit 1)", err)
        # odin3 rejected the file: Yosys reads the source once more, to tell a crash of its own
        self.assertEqual(self.names(), ["yosys", "odin3", "yosys"])

    def test_yosys_crash_is_an_oracle_crash(self) -> None:
        """Task 26 (PR 2D-3 review m6): Yosys 0.55 segfaults on recursive instantiation, where
        odin3 reports the recursion. Exit 3, naming both; never exit 1 or 2."""
        signal_b = {"FAKE_YOSYS_SIGNAL": "11", "FAKE_YOSYS_SIGNAL_ON": "read_verilog -sv"}
        code, out, err = self.run_tool(extra={"FAKE_ODIN3_RC": "1", **signal_b})
        self.assertEqual(code, 3, err)
        self.assertIn("oracle crash", out)
        self.assertIn("signal 11", out)
        self.assertIn("odin3 rejected the file: odin3: error: no", out)
        code, out, err = self.run_tool(extra=signal_b)  # odin3 accepts, Yosys crashes
        self.assertEqual(code, 3, err)
        self.assertIn("Yosys side B crashed", out)
        # a crash while Yosys reads odin3's own output is not the oracle's
        code, _, err = self.run_tool(
            extra={"FAKE_YOSYS_SIGNAL": "11", "FAKE_YOSYS_SIGNAL_ON": "read_rtlil a.il; hierarchy"})
        self.assertEqual(code, 2)
        self.assertIn("Yosys side A failed (exit -11)", err)
        # odin3 rejects and Yosys accepts: still a 2D rejection
        code, _, _ = self.run_tool(extra={"FAKE_ODIN3_RC": "1"})
        self.assertEqual(code, 2)

    def test_a_child_killed_by_its_cap_is_out_of_memory(self) -> None:
        code, _, err = self.run_tool("--mem-cap", str(4 * GIB),
                                     extra={"FAKE_YOSYS_SIGNAL": "9",
                                            "FAKE_YOSYS_SIGNAL_ON": "read_verilog -sv"})
        self.assertEqual(code, 2)
        self.assertIn("out of memory (cap 4G)", err)
        self.assertEqual(elab_equiv.format_cap(512 << 20), "512M")

    def test_yosys_failures_are_errors(self) -> None:
        for fail, what in (("read_rtlil a.il; hierarchy", "Yosys side A"),
                           ("read_verilog -sv", "Yosys side B"),
                           ("write_rtlil b.il", "Yosys's read_rtlil of a.il")):
            self.log.unlink(missing_ok=True)
            code, _, err = self.run_tool(extra={"FAKE_YOSYS_FAIL": fail})
            self.assertEqual(code, 2, fail)
            self.assertIn(f"{what} failed", err)
            self.assertNotIn("equiv-check", self.names())

    def test_missing_file(self) -> None:
        self.src.unlink()
        code, _, err = self.run_tool()
        self.assertEqual(code, 2)
        self.assertIn("is not a file", err)

    # -- --a-only --------------------------------------------------------------------------

    def test_a_only_stops_after_a_blif(self) -> None:
        code, out, err = self.run_tool("--a-only", "--top", "t")
        self.assertEqual(code, 0, out + err)
        self.assertTrue((self.keep / "a.blif").is_file())
        self.assertFalse((self.keep / "b.blif").exists())
        self.assertEqual(self.names(), ["yosys", "odin3", "yosys"])
        self.assertNotIn("equiv-check", self.names())

    # -- the version -----------------------------------------------------------------------

    def test_other_version_refused(self) -> None:
        code, _, err = self.run_tool(extra={"FAKE_YOSYS_VERSION": "Yosys 0.69 (git sha1 abc)"})
        self.assertEqual(code, 2)
        self.assertIn("not Yosys 0.55", err)
        self.assertEqual(self.names(), ["yosys"])

    def test_other_version_allowed_by_name(self) -> None:
        code, out, err = self.run_tool("--allow-version", "0.69",
                                       extra={"FAKE_YOSYS_VERSION": "Yosys 0.69 (git sha1 abc)"})
        self.assertEqual(code, 0, out + err)

    def test_allow_version_does_not_admit_others(self) -> None:
        code, _, _ = self.run_tool("--allow-version", "0.69",
                                   extra={"FAKE_YOSYS_VERSION": "Yosys 0.70 (git sha1 abc)"})
        self.assertEqual(code, 2)

    def test_version_oracle_matches_055_not_0550(self) -> None:
        self.assertTrue(elab_equiv.version_ok("Yosys 0.55 (git)", None))
        self.assertTrue(elab_equiv.version_ok("Yosys 0.55", None))
        self.assertFalse(elab_equiv.version_ok("Yosys 0.555", None))
        self.assertFalse(elab_equiv.version_ok("Yosys 0.5", None))

    # -- the enable-free back end (2D Task 24's memory bridge) -------------------------------

    def test_unmap_enables_changes_only_the_flop_steps(self) -> None:
        plain = elab_equiv.yosys_script("read_verilog a.v", "t", "x.blif")
        unmapped = elab_equiv.yosys_script("read_verilog a.v", "t", "x.blif", unmap=True)
        self.assertNotIn("dffunmap", plain)
        self.assertNotIn("-nodffe", plain)
        self.assertIn("dffunmap; techmap", unmapped)
        self.assertEqual(unmapped.count("-nodffe -nosdff"), 4)
        self.assertEqual(plain, unmapped.replace(" -nodffe -nosdff", "").replace("dffunmap; ", ""))

    def test_async2sync_adds_one_step_before_the_flop_mapping(self) -> None:
        plain = elab_equiv.yosys_script("read_verilog a.v", "t", "x.blif")
        a2s = elab_equiv.yosys_script("read_verilog a.v", "t", "x.blif", async2sync=True)
        self.assertNotIn("async2sync", plain)
        self.assertIn("memory_map; opt -full; async2sync; techmap", a2s)
        self.assertEqual(plain, a2s.replace("async2sync; ", ""))
        both = elab_equiv.yosys_script("read_verilog a.v", "t", "x.blif", unmap=True,
                                       async2sync=True)
        self.assertIn("async2sync; dffunmap; techmap", both)

    def test_async2sync_flag_reaches_both_sides(self) -> None:
        self.run_tool("--async2sync", "--top", "t")
        scripts = [c[2] for c in self.calls() if c[0] == "yosys" and "-p" in c]
        self.assertEqual(sum("async2sync; techmap" in s for s in scripts), 2)

    # -- the cap ---------------------------------------------------------------------------

    def test_every_child_goes_through_memcap(self) -> None:
        seen: list[tuple[list[str], int]] = []
        real = memcap.run

        def spy(argv: list[str], cap: int, capture_output: bool = False, **kw: object):  # type: ignore[no-untyped-def]
            seen.append((list(argv), cap))
            return real(argv, cap, capture_output, **kw)  # type: ignore[arg-type]

        with mock.patch.object(memcap, "run", spy):
            code, _, _ = self.run_tool("--mem-cap", str(2 * GIB))
        self.assertEqual(code, 0)
        self.assertEqual(len(seen), 6)
        self.assertTrue(all(cap == 2 * GIB for _, cap in seen))

    def test_default_cap_is_four_gib(self) -> None:
        caps: list[int] = []
        real = memcap.run

        def spy(argv: list[str], cap: int, capture_output: bool = False, **kw: object):  # type: ignore[no-untyped-def]
            caps.append(cap)
            return real(argv, cap, capture_output, **kw)  # type: ignore[arg-type]

        with mock.patch.object(memcap, "run", spy):
            self.run_tool()
        self.assertEqual(set(caps), {4 * GIB})

    def test_cap_above_sixty_percent_is_refused(self) -> None:
        code, _, err = self.run_tool("--mem-cap", str(30 * GIB))
        self.assertEqual(code, 2)
        self.assertIn("memcap refused", err)
        self.assertEqual(self.calls(), [])

    def test_asan_child_without_systemd_run_skips_with_77(self) -> None:
        _exe(self.bin / "odin3", FAKE_ODIN3 + "# __asan_init\n")
        code, _, err = self.run_tool()
        self.assertEqual(code, 77)
        self.assertIn("skipped", err)
        self.assertIn("ASan", err)


class MemcapTest(unittest.TestCase):
    """memcap.run: the systemd-run wrapper, prlimit, the ASan skip and the cap limit."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        self.log = self.dir / "sd.log"
        meminfo = self.dir / "meminfo"
        meminfo.write_text(MEMINFO_32G, encoding="ascii")
        env = {"FAKE_SYSTEMD_LOG": str(self.log), "PATH": str(self.bin)}
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.dict(os.environ, env))
        stack.enter_context(mock.patch.object(memcap, "MEMINFO", str(meminfo)))
        self.addCleanup(stack.close)
        os.environ.pop("ODIN3_HEAVY_SLICE", None)
        self.child = [sys.executable, "-c", "pass"]
        self.talk = [sys.executable, "-c", "print('hello')"]

    def logged(self) -> list[list[str]]:
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def test_systemd_run_wraps_the_child(self) -> None:
        _exe(self.bin / "systemd-run", FAKE_SYSTEMD_RUN)
        done = memcap.run(self.talk, 4 * GIB, capture_output=True)
        self.assertEqual(done.returncode, 0)
        self.assertEqual(done.stdout, "hello\n")
        self.assertEqual(self.logged(), [["systemd-run", "--user", "--scope", "-q", "-p",
                                          f"MemoryMax={4 * GIB}", "-p", "MemorySwapMax=0",
                                          *self.talk]])

    def test_heavy_slice_is_used(self) -> None:
        _exe(self.bin / "systemd-run", FAKE_SYSTEMD_RUN)
        with mock.patch.dict(os.environ, {"ODIN3_HEAVY_SLICE": "odin3-heavy.slice"}):
            memcap.run(self.child, GIB)
        self.assertEqual(self.logged()[0][:6], ["systemd-run", "--user", "--scope", "-q",
                                                "--slice=odin3-heavy.slice", "-p"])

    def test_output_is_not_captured_unless_asked(self) -> None:
        _exe(self.bin / "systemd-run", FAKE_SYSTEMD_RUN)
        done = memcap.run(self.child, GIB)
        self.assertIsNone(done.stdout)

    def test_a_failing_child_is_returned_not_raised(self) -> None:
        _exe(self.bin / "systemd-run", FAKE_SYSTEMD_RUN)
        done = memcap.run([sys.executable, "-c", "import sys; sys.exit(7)"], GIB)
        self.assertEqual(done.returncode, 7)

    def test_prlimit_for_a_release_child(self) -> None:
        _exe(self.bin / "prlimit", FAKE_PRLIMIT)
        done = memcap.run(self.talk, 4 * GIB, capture_output=True)
        self.assertEqual(done.returncode, 0)
        self.assertEqual(self.logged(), [["prlimit", f"--as={4 * GIB}", *self.talk]])

    def test_rlimit_when_there_is_no_prlimit(self) -> None:
        code = "import resource; print(resource.getrlimit(resource.RLIMIT_AS))"
        done = memcap.run([sys.executable, "-c", code], 4 * GIB, capture_output=True)
        self.assertEqual(done.stdout.strip(), f"({4 * GIB}, {4 * GIB})")

    def test_asan_child_without_systemd_run(self) -> None:
        fake = _exe(self.dir / "asan-child", "#!{python}\n# __asan_init\nprint('x')\n")
        with self.assertRaises(memcap.CapUnavailable):
            memcap.run([str(fake)], GIB)

    def test_asan_child_with_systemd_run_runs(self) -> None:
        _exe(self.bin / "systemd-run", FAKE_SYSTEMD_RUN)
        fake = _exe(self.dir / "asan-child", "#!{python}\n# __asan_init\nprint('x')\n")
        done = memcap.run([str(fake)], GIB, capture_output=True)
        self.assertEqual(done.stdout, "x\n")

    def test_is_asan_scans_the_whole_file(self) -> None:
        big = self.dir / "big"
        big.write_bytes(b"\0" * (memcap.SCAN_CHUNK + 7) + b"__asan_init" + b"\0" * 5)
        self.assertTrue(memcap.is_asan(str(big)))
        plain = self.dir / "plain"
        plain.write_bytes(b"\0" * (memcap.SCAN_CHUNK + 7))
        self.assertFalse(memcap.is_asan(str(plain)))
        edge = self.dir / "edge"
        edge.write_bytes(b"\0" * (memcap.SCAN_CHUNK - 5) + b"__asan_init")  # across two chunks
        self.assertTrue(memcap.is_asan(str(edge)))

    def test_cap_above_sixty_percent_of_memtotal(self) -> None:
        limit = 32 * GIB * 60 // 100
        memcap.check_cap(limit)
        with self.assertRaises(ValueError):
            memcap.check_cap(limit + 1)
        with self.assertRaises(ValueError):
            memcap.run(self.child, 30 * GIB)

    def test_bad_caps(self) -> None:
        for cap in (0, -1):
            with self.assertRaises(ValueError):
                memcap.check_cap(cap)

    def test_no_meminfo(self) -> None:
        absent = mock.patch.object(memcap, "MEMINFO", str(self.dir / "absent"))
        with absent, self.assertRaises(RuntimeError):
            memcap.check_cap(GIB)

    def test_lock_descriptor_is_passed_on(self) -> None:
        code = "import os\ntry:\n os.fstat(9)\n print('open')\nexcept OSError:\n print('closed')"
        saved = None
        try:
            saved = os.dup(9)
        except OSError:
            saved = None
        null = os.open(os.devnull, os.O_RDONLY)
        os.dup2(null, memcap.LOCK_FD)
        os.close(null)
        try:
            done = memcap.run([sys.executable, "-c", code], GIB, capture_output=True)
        finally:
            if saved is not None:
                os.dup2(saved, memcap.LOCK_FD)
                os.close(saved)
            else:
                os.close(memcap.LOCK_FD)
        self.assertEqual(done.stdout.strip(), "open")
        done = memcap.run([sys.executable, "-c", code], GIB, capture_output=True)
        self.assertEqual(done.stdout.strip(), "closed")

    def test_rlimit_is_restored_in_this_process(self) -> None:
        before = resource.getrlimit(resource.RLIMIT_AS)
        memcap.run(self.child, GIB, capture_output=True)
        self.assertEqual(resource.getrlimit(resource.RLIMIT_AS), before)

    def test_subprocess_error_is_an_oserror(self) -> None:
        with self.assertRaises(OSError):
            memcap.run([str(self.dir / "no-such-binary")], GIB)
        self.assertTrue(isinstance(subprocess.CompletedProcess, type))


if __name__ == "__main__":
    unittest.main()

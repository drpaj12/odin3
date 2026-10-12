"""Tests for tools/parse-check/parse-corpus (2C spec §7.4, plan Task 10) over a fake driver.

The fake odin3-parse-check appends the file it was given to calls.log, fails when the file's
text contains FAIL and sleeps when it contains SLOW; the fake odin3-write appends its arguments
to calls.log and copies the BLIF's text to the Verilog path. The heavy tests run parse-corpus as
its sweep CTests do, under the real tools/heavy-run with the real flock(1) and fakes of
`systemctl` and `systemd-run` (which runs the job in place) first on PATH, ODIN3_HEAVY_DIR and
ODIN3_HEAVY_CGROUP pointing into the test's directory: parse-corpus has no heavy logic of its own
(Task 10 re-review R1/R2), so heavy-run alone decides skip, refusal and nesting.
"""

from __future__ import annotations

import os
import re
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import parse_corpus

from . import helpers

FAKE_CHECK = """#!{python}
import os, sys
path = sys.argv[1]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("check " + os.path.basename(path) + "\\n")
text = open(path).read()
if "SLOW" in text:
    import time
    time.sleep(30)
if "FAIL" in text:
    sys.stderr.write("x.v:1:1: error: syntax: boom\\nFAIL " + path + "\\n")
    sys.exit(1)
"""

FAKE_WRITER = """#!{python}
import os, shutil, sys
out, blif = sys.argv[2], sys.argv[3]
assert sys.argv[1] == "--verilog"
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("write " + os.path.basename(blif) + "\\n")
shutil.copy(blif, out)
"""

# The heavy-run fakes: systemctl succeeds; systemd-run logs its --slice and runs the job in place.
FAKE_SYSTEMCTL = """#!{python}
"""

FAKE_SYSTEMD_RUN = """#!{python}
import os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("systemd-run " + next(a for a in args if a.startswith("--slice=")) + "\\n")
at = args.index("OOMPolicy=continue") + 1
os.execvp(args[at], args[at:])
"""

# Outside any heavy slice, and in user.slice itself (R1's crafted slice).
CGROUP_OUT = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/run-r0.scope\n"
CGROUP_USER = "0::/user.slice/run-r1.scope\n"

TOOL = Path(parse_corpus.__file__).resolve()
HEAVY_RUN = helpers.REPO_ROOT / "tools" / "heavy-run"
SWEEPS = ("verilog_micro_parse", "verilog_writer_parse", "verilog_vtr_parse")


class ParseCorpusTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.corpus = self.dir / "corpus"
        (self.corpus / "sub").mkdir(parents=True)
        self.log = self.dir / "calls.log"
        self.check = self.fake("check", FAKE_CHECK)
        self.writer = self.fake("writer", FAKE_WRITER)
        patcher = mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log)})
        patcher.start()
        self.addCleanup(patcher.stop)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def fake(self, name: str, text: str) -> str:
        path = self.dir / name
        path.write_text(text.format(python=sys.executable))
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return str(path)

    def put(self, rel: str, text: str) -> None:
        (self.corpus / rel).write_text(text)

    def expect(self, text: str) -> str:
        path = self.dir / "expect.txt"
        path.write_text(text)
        return str(path)

    def run_tool(self, *args: str) -> tuple[int, str, str]:
        return helpers.run_main(parse_corpus.main, ["--check", self.check, *args])

    def calls(self) -> list[str]:
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_files_run_smallest_first(self) -> None:
        self.put("big.v", "module big; wire a, b, c, d; endmodule\n")
        self.put("sub/small.v", "module s; endmodule\n")
        self.put("mid.v", "module mid; wire a; endmodule\n")
        self.put("notes.txt", "not verilog\n")
        code, out, _ = self.run_tool(str(self.corpus))
        self.assertEqual(code, 0, out)
        self.assertEqual(self.calls(), ["check small.v", "check mid.v", "check big.v"])
        self.assertIn("3 files, 3 passed", out)

    def test_an_unlisted_failure_exits_1(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        self.put("sub/b.v", "FAIL\n")
        code, out, _ = self.run_tool(str(self.corpus))
        self.assertEqual(code, 1)
        self.assertIn("FAIL  sub/b.v", out)
        self.assertIn("syntax: boom", out)
        self.assertIn("1 unexpected failures", out)

    def test_a_listed_pass_exits_1(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        code, out, _ = self.run_tool(
            "--expect-fail", self.expect("a.v Parmys rejects it\n"), str(self.corpus)
        )
        self.assertEqual(code, 1)
        self.assertIn("XPASS a.v", out)

    def test_a_listed_failure_exits_0(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        self.put("sub/b.v", "FAIL\n")
        listed = self.expect("# a comment\n\nsub/b.v SystemVerilog named arguments\n")
        code, out, _ = self.run_tool("--expect-fail", listed, str(self.corpus))
        self.assertEqual(code, 0, out)
        self.assertIn("xfail sub/b.v", out)
        self.assertIn("SystemVerilog named arguments", out)
        self.assertIn("1 expected failures", out)

    def test_a_stale_entry_exits_1(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        code, out, _ = self.run_tool(
            "--expect-fail", self.expect("gone.v it was removed\n"), str(self.corpus)
        )
        self.assertEqual(code, 1)
        self.assertIn("STALE gone.v", out)

    def test_reasons_are_required(self) -> None:
        self.put("sub/b.v", "FAIL\n")
        code, _, err = self.run_tool("--expect-fail", self.expect("sub/b.v\n"), str(self.corpus))
        self.assertEqual(code, 2)
        self.assertIn("has no reason", err)

    def test_the_writer_runs_once_per_blif(self) -> None:
        self.put("one.blif", "module one; endmodule\n")
        self.put("sub/two.blif", "module two; wire w; endmodule\n")
        self.put("skip.v", "FAIL\n")
        code, out, _ = self.run_tool(
            "--writer", self.writer, "--tmp", str(self.dir / "tmp"), str(self.corpus)
        )
        self.assertEqual(code, 0, out)
        self.assertEqual(
            self.calls(), ["write one.blif", "check one.v", "write two.blif", "check two.v"]
        )

    def test_root_makes_file_arguments_relative(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        self.put("sub/b.v", "FAIL\n")
        listed = self.expect("sub/b.v listed under the root\n")
        code, out, _ = self.run_tool(
            "--root",
            str(self.corpus),
            "--expect-fail",
            listed,
            str(self.corpus / "a.v"),
            str(self.corpus / "sub" / "b.v"),
        )
        self.assertEqual(code, 0, out)

    def run_heavy(
        self, cgroup: str = CGROUP_OUT, fd9: bool = False, **env: str
    ) -> subprocess.CompletedProcess[str]:
        """tools/heavy-run python parse_corpus.py --check <fake> <corpus>, as odin3_add_sweep_test
        registers it, in cgroup, with the heavy lock opened (not locked) on fd 9 if fd9."""
        bin_dir = self.dir / "bin"
        bin_dir.mkdir(exist_ok=True)
        for name, text in (("systemctl", FAKE_SYSTEMCTL), ("systemd-run", FAKE_SYSTEMD_RUN)):
            path = bin_dir / name
            path.write_text(text.format(python=sys.executable))
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        heavy = self.dir / "heavy"
        heavy.mkdir(exist_ok=True)
        cgroup_file = self.dir / "cgroup"
        cgroup_file.write_text(cgroup)
        base = {k: v for k, v in os.environ.items() if not k.startswith("ODIN3_HEAVY")}
        base.update({"PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
                     "ODIN3_HEAVY_DIR": str(heavy), "ODIN3_HEAVY_CGROUP": str(cgroup_file),
                     "ODIN3_HEAVY_WAIT": "5", **env})  # fmt: skip
        opener = f'exec 9>>"{heavy / "odin3-heavy.lock"}"; ' if fd9 else ""
        cmd = ["bash", "-c", f'{opener}exec "$@"', "job", str(HEAVY_RUN), sys.executable,
               str(TOOL), "--check", self.check, str(self.corpus)]  # fmt: skip
        return subprocess.run(
            cmd, env=base, capture_output=True, text=True, timeout=60, check=False
        )

    def test_the_sweep_ctests_go_through_odin3_add_sweep_test(self) -> None:
        text = (helpers.REPO_ROOT / "CMakeLists.txt").read_text()
        for name in SWEEPS:
            self.assertRegex(text, rf"odin3_add_sweep_test\({name} LABEL sweep\b")
            self.assertNotRegex(text, rf"add_test\(NAME {name}\b")

    def test_heavy_logic_is_heavy_runs_alone(self) -> None:
        """No --heavy, no lock, slice or nesting check in parse-corpus (R1/R2 removed by design)."""
        self.put("a.v", "module a; endmodule\n")
        code, _, err = self.run_tool("--heavy", str(self.corpus))
        self.assertEqual(code, 2)
        self.assertIn("unrecognized arguments: --heavy", err)
        self.assertEqual(self.calls(), [])
        self.assertIsNone(re.search(r"ODIN3_HEAVY|flock|systemd-run|cgroup", TOOL.read_text()))

    def test_heavy_skips_without_the_go_ahead(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        proc = self.run_heavy()
        self.assertEqual(proc.returncode, 77, proc.stdout + proc.stderr)
        self.assertIn("ODIN3_HEAVY_OK is not 1; skipped", proc.stderr)
        self.assertEqual(self.calls(), [])

    def test_heavy_sweeps_in_the_slice(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        proc = self.run_heavy(ODIN3_HEAVY_OK="1")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.calls(), ["systemd-run --slice=odin3-heavy.slice", "check a.v"])
        self.assertIn("1 files, 1 passed", proc.stdout)
        self.put("b.v", "FAIL\n")
        self.assertEqual(self.run_heavy(ODIN3_HEAVY_OK="1").returncode, 1)  # the sweep's status

    def test_r1_a_crafted_slice_and_an_unlocked_fd_never_run_uncapped(self) -> None:
        """R1: ODIN3_HEAVY_SLICE=user.slice, a user.slice cgroup and the lock file open (not
        locked) on fd 9 used to run in place, uncapped. heavy-run ignores the variable; the fd
        outside a heavy slice is refused at once (R2), and without the go-ahead it skips."""
        self.put("a.v", "module a; endmodule\n")
        for slice_var in ("user.slice", "odin3-heavy.slice"):
            proc = self.run_heavy(CGROUP_USER, fd9=True, ODIN3_HEAVY_SLICE=slice_var)
            self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
            self.assertIn("run the sweep outside the build", proc.stderr)
            proc = self.run_heavy(CGROUP_USER, ODIN3_HEAVY_SLICE=slice_var)
            self.assertEqual(proc.returncode, 77, proc.stdout + proc.stderr)
            self.assertIn(f"ODIN3_HEAVY_SLICE={slice_var} is set outside a heavy run",
                          proc.stderr)  # fmt: skip
        self.assertEqual(self.calls(), [])  # never swept, never wrapped

    def test_a_timeout_is_that_files_failure(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        self.put("sub/slow.v", "SLOW module s; endmodule\n")
        with mock.patch.object(parse_corpus, "TIMEOUT_S", 1):
            code, out, _ = self.run_tool(str(self.corpus))
        self.assertEqual(code, 1)
        self.assertIn("FAIL  sub/slow.v", out)
        self.assertIn("timed out after 1 s", out)
        self.assertIn("2 files, 1 passed", out)

    def test_written_verilog_over_the_source_cap_fails_unchecked(self) -> None:
        self.put("big.blif", "module big; wire a, b, c, d; endmodule\n")
        tmp = self.dir / "tmp"
        with mock.patch.object(parse_corpus, "SOURCE_CAP", 16):
            code, out, _ = self.run_tool(
                "--writer", self.writer, "--tmp", str(tmp), str(self.corpus)
            )
        self.assertEqual(code, 1)
        self.assertIn("over 2B's source-file cap of 16", out)
        self.assertEqual(self.calls(), ["write big.blif"])
        self.assertEqual(list(tmp.iterdir()), [])

    def test_missing_input_and_empty_corpus_exit_2(self) -> None:
        self.assertEqual(self.run_tool(str(self.dir / "nope"))[0], 2)
        self.assertEqual(self.run_tool(str(self.corpus))[0], 2)


if __name__ == "__main__":
    unittest.main()

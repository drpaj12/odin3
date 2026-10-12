"""Tests for tools/parse-check/parse-corpus (2C spec §7.4, plan Task 10) over a fake driver.

The fake odin3-parse-check appends the file it was given to calls.log, fails when the file's
text contains FAIL and sleeps when it contains SLOW; the fake odin3-write appends its arguments
to calls.log and copies the BLIF's text to the Verilog path. The heavy-run tests put a fake
`flock` and `systemd-run` first on PATH: each logs its arguments and runs the command it wraps.
"""

from __future__ import annotations

import os
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

# flock <lock> <cmd...>; systemd-run <options...> -p MemorySwapMax=0 <cmd...>
FAKE_WRAPPER = """#!{python}
import os, sys
name = os.path.basename(sys.argv[0])
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(" ".join([name, *sys.argv[1:3]]) + " ...\\n")
    if name == "systemd-run":
        log.write("argv " + " ".join(sys.argv[1:8]) + "\\n")
        log.write("slice " + os.environ.get("ODIN3_HEAVY_SLICE", "unset") + "\\n")
args = sys.argv[1:]
rest = args[1:] if name == "flock" else args[args.index("MemorySwapMax=0") + 1:]
os.execvp(rest[0], rest)
"""

TOOL = Path(parse_corpus.__file__).resolve()


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

    def test_heavy_skips_without_the_go_ahead(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        with mock.patch.dict(os.environ, {"ODIN3_HEAVY_OK": ""}):
            code, out, _ = self.run_tool("--heavy", str(self.corpus))
        self.assertEqual(code, 77)
        self.assertEqual(self.calls(), [])
        with mock.patch.dict(os.environ, {"ODIN3_HEAVY_OK": "1", "ODIN3_HEAVY_SLICE": "1"}):
            code, out, _ = self.run_tool("--heavy", str(self.corpus))
        self.assertEqual(code, 0, out)

    def run_heavy(self, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
        """parse-corpus --heavy as a process, with the fakes first on PATH."""
        bin_dir = self.dir / "bin"
        bin_dir.mkdir(exist_ok=True)
        for name in ("flock", "systemd-run"):
            path = bin_dir / name
            path.write_text(FAKE_WRAPPER.format(python=sys.executable))
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        base = {k: v for k, v in os.environ.items() if not k.startswith("ODIN3_HEAVY")}
        base.update({"PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}", "ODIN3_HEAVY_OK": "1"})
        base.update(env)
        cmd = [sys.executable, str(TOOL), "--check", self.check, "--heavy", str(self.corpus)]
        return subprocess.run(cmd, env=base, capture_output=True, text=True, check=False)

    def test_heavy_runs_under_flock_and_the_slice(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        proc = self.run_heavy({})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(
            self.calls(),
            [
                "flock /tmp/claude-1000/odin3-heavy.lock systemd-run ...",
                "systemd-run --user --scope ...",
                "argv --user --scope -q --slice=odin3-heavy.slice -p MemoryMax=6G -p",
                "slice 1",
                "check a.v",
            ],
        )
        self.assertIn("1 files, 1 passed", proc.stdout)

    def test_heavy_run_prefix_can_be_replaced(self) -> None:
        self.put("a.v", "FAIL\n")
        proc = self.run_heavy({"ODIN3_HEAVY_RUN": ""})
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.calls(), ["check a.v"])
        flock = self.dir / "bin" / "flock"
        proc = self.run_heavy({"ODIN3_HEAVY_RUN": f"'{flock}' /tmp/other.lock"})
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.calls()[-2:], ["flock /tmp/other.lock " + sys.executable + " ...",
                                             "check a.v"])

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

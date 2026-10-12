"""Tests for tools/parse-check/parse-corpus (2C spec §7.4, plan Task 10) over a fake driver.

The fake odin3-parse-check appends the file it was given to calls.log and fails when the file's
text contains FAIL; the fake odin3-write appends its arguments to calls.log and copies the BLIF's
text to the Verilog path.
"""

from __future__ import annotations

import os
import stat
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
if "FAIL" in open(path).read():
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
        code, out, _ = self.run_tool("--writer", self.writer, str(self.corpus))
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
        with mock.patch.dict(os.environ, {"ODIN3_HEAVY_OK": "1"}):
            code, out, _ = self.run_tool("--heavy", str(self.corpus))
        self.assertEqual(code, 0, out)

    def test_missing_input_and_empty_corpus_exit_2(self) -> None:
        self.assertEqual(self.run_tool(str(self.dir / "nope"))[0], 2)
        self.assertEqual(self.run_tool(str(self.corpus))[0], 2)


if __name__ == "__main__":
    unittest.main()

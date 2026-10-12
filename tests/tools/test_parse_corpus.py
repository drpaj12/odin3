"""Tests for tools/parse-check/parse-corpus (2C spec §7.4, plan Task 10) over a fake driver.

The fake odin3-parse-check appends the file it was given to calls.log, fails when the file's
text contains FAIL and sleeps when it contains SLOW; the fake odin3-write appends its arguments
to calls.log and copies the BLIF's text to the Verilog path. The heavy-run tests put fakes of
`flock`, `systemd-run`, a 2E-contract `heavy-run` and a contract-breaking `plain` wrapper first
on PATH, with ODIN3_HEAVY_DIR and ODIN3_HEAVY_CGROUP pointing into the test's directory.
"""

from __future__ import annotations

import fcntl
import os
import stat
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Sequence
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

# The heavy-run fakes. flock: `flock -x -w <s> -E <code> <lock> <cmd...>`, holding the lock on an
# inherited descriptor (flock(1)'s child keeps it), <code> after <s> s busy. systemd-run: logs
# its options and ODIN3_HEAVY_SLICE, runs what follows `MemorySwapMax=0`. heavy-run: the 2E
# contract (the lock on an inherited descriptor, ODIN3_HEAVY_SLICE=odin3-heavy.slice exported).
# plain: a wrapper that breaks the contract (no lock, no slice).
FAKE_FLOCK = """#!{python}
import fcntl, os, sys, time
args = sys.argv[1:]
wait, code, lock, cmd = float(args[2]), int(args[4]), args[5], args[6:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(" ".join(["flock", *args[:5], os.path.basename(lock)]) + "\\n")
fd = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_APPEND)
deadline = time.monotonic() + wait
while True:
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        break
    except BlockingIOError:
        if time.monotonic() > deadline:
            sys.exit(code)
        time.sleep(0.05)
os.set_inheritable(fd, True)
os.execvp(cmd[0], cmd)
"""

FAKE_SYSTEMD_RUN = """#!{python}
import os, sys
args = sys.argv[1:]
at = args.index("MemorySwapMax=0") + 1
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(" ".join(["systemd-run", *args[:at]]) + "\\n")
    log.write("slice " + os.environ.get("ODIN3_HEAVY_SLICE", "unset") + "\\n")
os.execvp(args[at], args[at:])
"""

FAKE_HEAVY_RUN = """#!{python}
import fcntl, os, sys
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("heavy-run\\n")
fd = os.open(os.path.join(os.environ["ODIN3_HEAVY_DIR"], "odin3-heavy.lock"),
             os.O_WRONLY | os.O_CREAT | os.O_APPEND)
fcntl.flock(fd, fcntl.LOCK_EX)
os.set_inheritable(fd, True)
os.environ["ODIN3_HEAVY_SLICE"] = "odin3-heavy.slice"
os.execvp(sys.argv[1], sys.argv[1:])
"""

FAKE_PLAIN = """#!{python}
import os, sys
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("plain\\n")
os.execvp(sys.argv[1], sys.argv[1:])
"""

FAKES = {
    "flock": FAKE_FLOCK,
    "systemd-run": FAKE_SYSTEMD_RUN,
    "heavy-run": FAKE_HEAVY_RUN,
    "plain": FAKE_PLAIN,
}

# A cgroup under the slice: with the cgroup override the lock alone tells inside from outside.
CGROUP = "0::/user.slice/user@1000.service/odin3-heavy.slice/run-1.scope\n"

WRAPPED_LOG = [
    "flock -x -w 14400 -E 77 odin3-heavy.lock",
    "systemd-run --user --scope -q --slice=odin3-heavy.slice -p MemoryMax=6G -p MemorySwapMax=0",
    "slice odin3-heavy.slice",
]

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

    def heavy_env(self) -> dict[str, str]:
        heavy = self.dir / "heavy"
        heavy.mkdir(exist_ok=True)
        cgroup = self.dir / "cgroup"
        cgroup.write_text(CGROUP)
        return {"ODIN3_HEAVY_DIR": str(heavy), "ODIN3_HEAVY_CGROUP": str(cgroup)}

    def test_heavy_skips_without_the_go_ahead(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        env = {**self.heavy_env(), "ODIN3_HEAVY_OK": "", "ODIN3_HEAVY_SLICE": ""}
        with mock.patch.dict(os.environ, env):
            code, out, _ = self.run_tool("--heavy", str(self.corpus))
        self.assertEqual(code, 77)
        self.assertEqual(self.calls(), [])

    def test_inside_a_heavy_run_it_sweeps_in_place(self) -> None:
        """In the slice with the heavy lock on a descriptor: no wrapper (in-process)."""
        self.put("a.v", "module a; endmodule\n")
        env = {**self.heavy_env(), "ODIN3_HEAVY_SLICE": "odin3-heavy.slice", "ODIN3_HEAVY_OK": ""}
        with mock.patch.dict(os.environ, env), open(parse_corpus.heavy_lock(), "a"):
            self.assertTrue(parse_corpus.inside_heavy_run())
            code, out, _ = self.run_tool("--heavy", str(self.corpus))
        self.assertEqual(code, 0, out)
        self.assertEqual(self.calls(), ["check a.v"])

    def run_heavy(
        self, env: dict[str, str], wrapper: Sequence[str] = ()
    ) -> subprocess.CompletedProcess[str]:
        """parse-corpus --heavy as a process (under wrapper, if any), the fakes first on PATH."""
        bin_dir = self.dir / "bin"
        bin_dir.mkdir(exist_ok=True)
        for name, text in FAKES.items():
            path = bin_dir / name
            path.write_text(text.format(python=sys.executable))
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        base = {k: v for k, v in os.environ.items() if not k.startswith("ODIN3_HEAVY")}
        base.update({"PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}", "ODIN3_HEAVY_OK": "1"})
        base.update(self.heavy_env())
        base.update(env)
        cmd = [*wrapper, sys.executable, str(TOOL), "--check", self.check, "--heavy",
               str(self.corpus)]  # fmt: skip
        return subprocess.run(
            cmd, env=base, capture_output=True, text=True, timeout=60, check=False
        )

    def test_heavy_runs_under_flock_and_the_slice(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        proc = self.run_heavy({})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.calls(), [*WRAPPED_LOG, "check a.v"])
        self.assertIn("1 files, 1 passed", proc.stdout)

    def test_under_a_2e_heavy_run_it_does_not_wrap_again(self) -> None:
        """odin3_add_sweep_test() runs the command under heavy-run: no second lock wait."""
        self.put("a.v", "module a; endmodule\n")
        proc = self.run_heavy({}, wrapper=[str(self.dir / "bin" / "heavy-run")])
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.calls(), ["heavy-run", "check a.v"])

    def test_heavy_run_as_odin3_heavy_run_does_not_wrap_again(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        proc = self.run_heavy({"ODIN3_HEAVY_RUN": str(self.dir / "bin" / "heavy-run")})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.calls(), ["heavy-run", "check a.v"])

    def test_a_stale_heavy_slice_is_still_wrapped(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        proc = self.run_heavy({"ODIN3_HEAVY_SLICE": "odin3-heavy.slice"})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("ODIN3_HEAVY_SLICE=odin3-heavy.slice is set outside a heavy run", proc.stderr)
        self.assertEqual(self.calls(), [*WRAPPED_LOG, "check a.v"])

    def test_an_empty_heavy_run_is_still_wrapped(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        proc = self.run_heavy({"ODIN3_HEAVY_RUN": ""})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("ODIN3_HEAVY_RUN is empty; ignoring it", proc.stderr)
        self.assertEqual(self.calls(), [*WRAPPED_LOG, "check a.v"])

    def test_a_wrapper_that_breaks_the_contract_is_not_wrapped_again(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        proc = self.run_heavy({"ODIN3_HEAVY_RUN": str(self.dir / "bin" / "plain")})
        self.assertEqual(proc.returncode, 2)
        self.assertIn("did not put this run inside a heavy run", proc.stderr)
        self.assertEqual(self.calls(), ["plain"])

    def test_a_busy_lock_skips_after_the_wait(self) -> None:
        self.put("a.v", "module a; endmodule\n")
        env = self.heavy_env()
        with open(Path(env["ODIN3_HEAVY_DIR"]) / "odin3-heavy.lock", "a") as held:
            fcntl.flock(held, fcntl.LOCK_EX)
            proc = self.run_heavy({"ODIN3_HEAVY_WAIT": "0.2"})
        self.assertEqual(proc.returncode, 77, proc.stdout + proc.stderr)
        self.assertEqual(self.calls(), ["flock -x -w 0.2 -E 77 odin3-heavy.lock"])

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

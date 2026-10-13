"""Tests for tools/elab-check/elab-sweep (2D Task 26; spec §14.5, plan points (n), (o), (t)), over
fake odin3, bench and elab-equiv scripts."""

from __future__ import annotations

import contextlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

import bench_report
import elab_sweep
import memcap

from . import helpers

GIB = 1 << 30
MEMINFO_32G = "MemTotal:       33554432 kB\nMemFree:        1000 kB\n"
MEMINFO_24G = "MemTotal:       25165824 kB\nMemFree:        1000 kB\n"

# The fake odin3 logs its argv; a source holding KILL ends it with SIGKILL (a cgroup kill), BAD
# makes it report an error.
FAKE_ODIN3 = """#!{python}
import json, os, signal, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["odin3", *argv]) + "\\n")
text = open(argv[-1]).read()
if "KILL" in text:
    os.kill(os.getpid(), signal.SIGKILL)
if "SEGV" in text:
    print("odin3: error: boom at line 3", file=sys.stderr)
    sys.stderr.flush()
    os.kill(os.getpid(), signal.SIGSEGV)
if "BAD" in text:
    print("odin3: error: boom at line 3", file=sys.stderr)
    sys.exit(1)
print("odin3: info: stats: done")
"""

FAKE_BENCH = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["bench", *argv]) + "\\n")
print("bytes=" + str(os.path.getsize(argv[1])) + " parse_ms=1")
"""

# Stands in for elab_equiv.py: the exit code is the one a source asks for (EQ1, EQ2, EQ3, EQ77).
FAKE_EQUIV = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["elab-equiv", *argv]) + "\\n")
text = open(argv[-1]).read()
if "EQOOM" in text:
    print("elab-equiv: error: Yosys (side B) failed: out of memory (cap 4G)\\nequiv-check failed",
          file=sys.stderr)
    sys.exit(2)
for code in (1, 2, 3, 77):
    if f"EQ{code}" in text:
        print(f"elab-equiv: message for exit {code}")
        sys.exit(code)
print("elab-equiv: equivalent")
"""


# Stands in for proc_diff.py: the exit code is the one a source asks for (PD1 differs, PD2 error,
# PD3 tie); PD2 prints a line a needle can match.
FAKE_PROC_DIFF = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["proc-diff", *argv]) + "\\n")
text = open(argv[-1]).read()
for code in (1, 2, 3):
    if f"PD{code}" in text:
        print(f"proc-diff: message for exit {code}: odin3: error: no latch allowed")
        sys.exit(code)
print("proc-diff: identical")
"""

# The opt-equiv / opt-diff stand-in (2F Task 17): exit 0 passes, 1 is a difference.
FAKE_OPT_TOOL = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["opt-tool", os.path.basename(sys.argv[0]), *argv]) + "\\n")
text = open(argv[-1]).read()
for code in (1, 2):
    if f"OT{code}" in text:
        print(f"opt-tool: message for exit {code}")
        sys.exit(code)
print("opt-tool: ok")
"""

# The opt-run stand-in (2F Task 18): prints the iteration count the file names (`IT<n>`).
FAKE_OPT_RUN = """#!{python}
import json, os, re, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["opt-tool", os.path.basename(sys.argv[0]), *argv]) + "\\n")
text = open(argv[-1]).read()
if "BAD" in text:
    print("opt-run: FAIL: no fixed point after 3 iterations (the cap)")
    sys.exit(1)
if "CRASH" in text:
    print("opt-run: CRASH: odin3 killed by signal 11")
    sys.exit(3)
print("opt-run: ok: iterations max=" + re.search(r"IT(\\d+)", text).group(1) + " opt-runs=2")
"""

# An odin3 on which `opt` exists: every run succeeds (the trio probe included).
FAKE_ODIN3_OPT = """#!{python}
import json, os, sys
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["odin3", *sys.argv[1:]]) + "\\n")
"""


def _exe(path: Path, text: str) -> Path:
    path.write_text(text.replace("{python}", sys.executable), encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class SweepTestCase(unittest.TestCase):
    """A temp tree with fakes; memcap runs children directly (no systemd-run on its path)."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        self.odin3 = _exe(self.bin / "odin3", FAKE_ODIN3)
        self.bench = _exe(self.bin / "bench_elab", FAKE_BENCH)
        self.fake_equiv = _exe(self.bin / "fake_elab_equiv.py", FAKE_EQUIV)
        self.fake_proc_diff = _exe(self.bin / "fake_proc_diff.py", FAKE_PROC_DIFF)
        self.odin3_opt = _exe(self.bin / "odin3_opt", FAKE_ODIN3_OPT)
        self.log = self.dir / "argv.log"
        self.meminfo = self.dir / "meminfo"
        self.meminfo.write_text(MEMINFO_32G, encoding="ascii")
        self.golden = self.dir / "golden"  # an empty tree unless a test fills it
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log)}))
        stack.enter_context(mock.patch.object(memcap, "MEMINFO", str(self.meminfo)))
        stack.enter_context(mock.patch.object(memcap, "SYSTEMD_RUN", "no-such-systemd-run"))
        stack.enter_context(mock.patch.object(elab_sweep, "ELAB_EQUIV", self.fake_equiv))
        stack.enter_context(mock.patch.object(elab_sweep, "PROC_DIFF", self.fake_proc_diff))
        self.addCleanup(stack.close)

    def write(self, rel: str, text: str = "module m; endmodule\n") -> Path:
        path = self.dir / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def run_tool(self, *args: str) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.odin3), "--golden-root", str(self.golden), *args]
        return helpers.run_main(elab_sweep.main, argv)

    def calls(self) -> list[list[str]]:
        if not self.log.is_file():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    def expect(self, text: str) -> str:
        return str(self.write("expect.txt", text))


class CollectTest(SweepTestCase):
    def test_one_ascending_size_order_across_two_directories(self) -> None:
        """Review I12: all files sorted together, not directory by directory."""
        a_big = self.write("a/big.v", "x" * 300)
        self.write("a/small.v", "x" * 10)
        self.write("b/mid.v", "x" * 100)
        self.write("b/tiny.v", "x" * 1)
        self.write("a/header.vh", "x" * 2)  # a header never runs alone
        code, out, err = self.run_tool(
            "--root", str(self.dir), str(self.dir / "a"), str(self.dir / "b"))
        self.assertEqual(code, 0, err)
        order = [line.split()[1] for line in out.splitlines() if line.startswith("ok")]
        self.assertEqual(order, ["b/tiny.v", "a/small.v", "b/mid.v", "a/big.v"])
        self.assertEqual([c[-1] for c in self.calls()][-1], str(a_big))
        self.assertNotIn("header.vh", out)
        self.assertIn("4 files, 4 passed", out)

    def test_collect_is_the_library_entry_point(self) -> None:
        self.write("a/x.v", "xx")
        one = self.write("b/y.v", "x")
        items = elab_sweep.collect([str(self.dir / "a"), str(one), str(one)])
        self.assertEqual([i.rel for i in items], ["y.v", "x.v"])
        self.assertEqual([i.size for i in items], [1, 2])
        with self.assertRaises(elab_sweep.UsageError):
            elab_sweep.collect([str(self.dir / "missing")])

    def test_split_xl_skips_listed_and_oversized_files(self) -> None:
        self.write("koios/lenet.v", "x" * 50)
        self.write("small.v", "x")
        self.write("mid.v", "x" * 20)
        items = elab_sweep.collect([str(self.dir)], self.dir)
        expect = elab_sweep.read_expect(Path(self.expect("koios/lenet.v xl: Yosys side 7 GB\n")))
        run, skipped = elab_sweep.split_xl(items, expect, xl=False)
        self.assertEqual(([i.rel for i in run], [i.rel for i in skipped]),
                         (["small.v", "mid.v"], ["koios/lenet.v"]))
        run, skipped = elab_sweep.split_xl(items, {}, xl=False, skip_over=30)
        self.assertEqual([i.rel for i in skipped], ["koios/lenet.v"])
        run, skipped = elab_sweep.split_xl(items, expect, xl=True)
        self.assertEqual([i.rel for i in run], ["koios/lenet.v"])


class LimitsTest(SweepTestCase):
    def test_jobs_times_cap_above_sixty_percent_is_refused(self) -> None:
        self.meminfo.write_text(MEMINFO_24G, encoding="ascii")
        self.write("a/x.v")
        code, _, err = self.run_tool("--jobs", "2", "--mem-cap", "10G", str(self.dir / "a"))
        self.assertEqual(code, 2)
        self.assertIn("--jobs 2 x --mem-cap 10G = 20G exceeds 60% of MemTotal (14.4G)", err)
        self.assertEqual(self.calls(), [])
        code, _, err = self.run_tool("--jobs", "1", "--mem-cap", "10G", str(self.dir / "a"))
        self.assertEqual(code, 0, err)

    def test_the_heavy_slice_bounds_jobs_too(self) -> None:
        self.write("a/x.v")
        with mock.patch.object(elab_sweep, "slice_limit", return_value=6 * GIB):
            code, _, err = self.run_tool("--jobs", "2", "--mem-cap", "4G", str(self.dir / "a"))
        self.assertEqual(code, 2)
        self.assertIn("heavy slice's MemoryMax (6G)", err)

    def test_a_child_killed_by_its_cap_is_out_of_memory(self) -> None:
        self.write("a/hog.v", "KILL\n")
        code, out, _ = self.run_tool(str(self.dir / "a"))
        self.assertEqual(code, 1)
        self.assertIn("FAIL  hog.v", out)
        self.assertIn("out of memory (cap 4G)", out)
        code, out, _ = self.run_tool("--mem-cap", "512M", str(self.dir / "a"))
        self.assertIn("out of memory (cap 512M)", out)

    def test_an_asan_binary_without_systemd_run_exits_77(self) -> None:
        asan = _exe(self.bin / "odin3-asan", "#!{python}\n# __asan_init\n")
        self.write("a/x.v")
        code, _, err = helpers.run_main(
            elab_sweep.main, ["--odin3", str(asan), str(self.dir / "a")])
        self.assertEqual(code, 77)
        self.assertIn("ASan", err)
        self.assertEqual(self.calls(), [])

    def test_a_larger_cap_for_one_file_runs_it_alone(self) -> None:
        self.write("a/small.v", "x")
        self.write("koios/lenet.v", "x" * 50)
        caps: list[int] = []
        real = memcap.run

        def spy(argv: list[str], cap: int, *a: object, **k: object) -> object:
            caps.append(cap)
            return real(argv, cap, *a, **k)  # type: ignore[arg-type]

        with mock.patch.object(memcap, "run", spy):
            code, out, err = self.run_tool(
                "--jobs", "2", "--cap-for", "koios/lenet.v=5G", "--root", str(self.dir),
                str(self.dir / "a"), str(self.dir / "koios"))
        self.assertEqual(code, 0, err)
        self.assertEqual(caps, [4 * GIB, 5 * GIB])
        self.assertLess(out.index("small.v"), out.index("lenet.v"))

    def test_techlib_run_still_elaborates_with_nolatches(self) -> None:
        item = elab_sweep.Item(self.write("a/x.v"), "a/x.v", 1)
        cfg = elab_sweep.Config(odin3=str(self.odin3), yosys=None, equiv_check=None, bench=None,
                                cap=GIB, caps={}, tops={}, golden_root=None,
                                techlib="/lib/t.o3lib")
        seen: list[list[str]] = []

        def spy(argv: list[str], cap: int, start: float) -> tuple[None, elab_sweep.Outcome]:
            seen.append(list(argv))
            return None, elab_sweep.Outcome("fail", 0.0, "spy")

        with mock.patch.object(elab_sweep, "run_child", spy):
            elab_sweep.run_elab(cfg, item, GIB)
        self.assertIn(f"read_techlib /lib/t.o3lib; read_verilog --nolatches {item.path}; stats",
                      seen[0])

    def test_a_cap_for_file_starts_with_nothing_else_in_flight(self) -> None:
        for n in range(4):
            self.write(f"a/s{n}.v", "x" * (n + 1))
        self.write("koios/lenet.v", "x" * 50)
        live = [0]
        at_start: dict[int, int] = {}
        lock = threading.Lock()
        def spy(argv: list[str], cap: int, *a: object, **k: object) -> object:
            with lock:
                live[0] += 1
                at_start[cap] = max(at_start.get(cap, 0), live[0])
            time.sleep(0.05)
            try:  # no real child: under load a real one can fail and hide the scheduling check
                return subprocess.CompletedProcess(argv, 0, "", "")
            finally:
                with lock:
                    live[0] -= 1

        with mock.patch.object(memcap, "run", spy):
            code, _, err = self.run_tool(
                "--jobs", "2", "--cap-for", "koios/lenet.v=5G", "--root", str(self.dir),
                str(self.dir / "a"), str(self.dir / "koios"))
        self.assertEqual(code, 0, err)
        self.assertEqual(at_start[5 * GIB], 1)

    def test_a_cap_for_file_is_checked_alone_against_the_slice(self) -> None:
        self.write("koios/lenet.v", "x")
        argv = ["--jobs", "2", "--mem-cap", "2G", "--cap-for", "koios/lenet.v=5G", "--root",
                str(self.dir), str(self.dir / "koios")]
        with mock.patch.object(elab_sweep, "slice_limit", return_value=4 * GIB):
            code, _, err = self.run_tool(*argv)
        self.assertEqual(code, 2)
        self.assertIn("--cap-for 5G (alone)", err)
        with mock.patch.object(elab_sweep, "slice_limit", return_value=6 * GIB):
            code, _, err = self.run_tool(*argv)
        self.assertEqual(code, 0, err)


class VerdictTest(SweepTestCase):
    def test_listed_pass_unlisted_failure_and_listed_failure(self) -> None:
        self.write("m/good.v", "ok\n")
        self.write("m/bad.v", "BAD\n")
        tree = str(self.dir / "m")
        # an unlisted failure fails the run
        code, out, _ = self.run_tool(tree)
        self.assertEqual(code, 1)
        self.assertIn("FAIL  bad.v", out)
        self.assertIn("exit 1: boom at line 3", out)
        # a listed failure passes
        listed = self.expect("bad.v 2C rejects it: boom\n")
        code, out, _ = self.run_tool("--expect-fail", listed, tree)
        self.assertEqual(code, 0, out)
        self.assertIn("xfail bad.v", out)
        self.assertIn("1 expected failures", out)
        # a listed pass fails the run
        both = self.expect("bad.v boom\ngood.v oops\n")
        code, out, _ = self.run_tool("--expect-fail", both, tree)
        self.assertEqual(code, 1)
        self.assertIn("XPASS good.v", out)

    def test_a_needle_must_appear_in_the_output_of_a_listed_failure(self) -> None:
        self.write("m/bad.v", "BAD\n")
        tree = str(self.dir / "m")
        code, out, _ = self.run_tool("--expect-fail", self.expect("bad.v [boom at line 3] why\n"),
                                     tree)
        self.assertEqual(code, 0, out)
        self.assertIn("xfail bad.v", out)
        # the wrong message: the file fails, but not as listed
        code, out, _ = self.run_tool("--expect-fail", self.expect("bad.v [other error] why\n"),
                                     tree)
        self.assertEqual(code, 1)
        self.assertIn("FAIL  bad.v", out)
        self.assertIn("expected [other error]", out)
        self.assertIn("1 unexpected failures", out)

    def test_a_signal_a_cap_kill_or_a_timeout_is_never_an_expected_failure(self) -> None:
        self.write("m/crash.v", "SEGV\n")
        tree = str(self.dir / "m")
        # even with a matching needle (the crash printed it first), or none at all
        for line in ("crash.v [boom at line 3] why\n", "crash.v why\n"):
            code, out, _ = self.run_tool("--expect-fail", self.expect(line), tree)
            self.assertEqual(code, 1, out)
            self.assertIn("FAIL  crash.v", out)
            self.assertIn("killed by signal 11", out)
            self.assertNotIn("xfail", out)
        self.write("k/killed.v", "KILL\n")
        code, out, _ = self.run_tool("--expect-fail", self.expect("killed.v [x] why\n"),
                                     str(self.dir / "k"))
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL  killed.v", out)
        self.assertIn("out of memory", out)
        self.write("t/slow.v", "SLOW\n")
        argv = ["--expect-fail", self.expect("slow.v [x] why\n"), str(self.dir / "t")]
        with mock.patch.object(memcap, "run",
                               side_effect=subprocess.TimeoutExpired("o", 1)):
            code, out, _ = self.run_tool(*argv)
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL  slow.v", out)
        self.assertIn("timed out", out)

    def test_the_needle_syntax_is_the_rows_list_syntax(self) -> None:
        path = Path(self.expect("a.v [x y] why\nb.v [n] xl: big\nc.v plain\n"))
        got = elab_sweep.read_expect(path)
        self.assertEqual(got["a.v"], elab_sweep.Expect("why", False, "x y"))
        self.assertEqual(got["b.v"], elab_sweep.Expect("xl: big", True, "n"))
        self.assertEqual(got["c.v"], elab_sweep.Expect("plain", False, ""))

    def test_a_stale_entry_fails_unless_the_run_is_partial(self) -> None:
        self.write("m/good.v")
        tree = str(self.dir / "m")
        stale = self.expect("gone.v it was deleted\n")
        code, out, _ = self.run_tool("--expect-fail", stale, tree)
        self.assertEqual(code, 1)
        self.assertIn("STALE gone.v", out)
        code, _, _ = self.run_tool("--expect-fail", stale, "--partial", tree)
        self.assertEqual(code, 0)

    def test_an_entry_needs_a_reason_and_may_not_repeat(self) -> None:
        self.write("m/good.v")
        tree = str(self.dir / "m")
        code, _, err = self.run_tool("--expect-fail", self.expect("good.v\n"), tree)
        self.assertEqual(code, 2)
        self.assertIn("has no reason", err)
        code, _, err = self.run_tool("--expect-fail", self.expect("good.v a\ngood.v b\n"), tree)
        self.assertEqual(code, 2)
        self.assertIn("listed twice", err)

    def test_the_xl_marker_is_a_run_class(self) -> None:
        self.write("k/big.v", "x" * 40)
        self.write("k/small.v", "x")
        tree = str(self.dir / "k")
        listed = self.expect("big.v xl: Yosys side 7 GB\n")
        code, out, _ = self.run_tool("--expect-fail", listed, tree)
        self.assertEqual(code, 0, out)
        self.assertIn("skip  big.v", out)
        self.assertEqual([c[-1] for c in self.calls()], [str(self.dir / "k/small.v")])
        self.log.unlink()
        code, out, _ = self.run_tool("--expect-fail", listed, "--xl", tree)
        self.assertEqual(code, 0, out)
        self.assertEqual([c[-1] for c in self.calls()], [str(self.dir / "k/big.v")])
        self.assertIn("1 passed", out)


class TopTest(SweepTestCase):
    def test_top_comes_from_the_first_model_of_the_parmys_golden(self) -> None:
        leaf = self.golden / "regression" / "verilog" / "keywords" / "assign" / "leaf1"
        leaf.mkdir(parents=True)
        (leaf / "leaf1.parmys.blif").write_text(".model topmod\n.end\n.model second\n.end\n")
        self.write("tests/micro/verilog/keywords/assign/leaf1.v")
        self.write("tests/micro/verilog/keywords/assign/nogolden.v")
        code, _, err = self.run_tool(str(self.dir / "tests/micro/verilog"))
        self.assertEqual(code, 0, err)
        by_file = {Path(c[-1]).name: c for c in self.calls()}
        self.assertEqual(by_file["leaf1.v"][1:4], ["--top", "topmod", "--elab-only"])
        self.assertNotIn("--top", by_file["nogolden.v"])

    def test_a_tops_file_wins_over_the_golden(self) -> None:
        self.write("m/a.v")
        self.write("m/b.v", "xx")
        tops = self.write("tops.txt", "# one per line\nb.v special\n")
        # keys are relative paths or file names; 'a.v' is unlisted
        code, _, err = self.run_tool("--tops", str(tops), str(self.dir / "m"))
        self.assertEqual(code, 0, err)
        by_file = {Path(c[-1]).name: c for c in self.calls()}
        self.assertEqual(by_file["b.v"][1:3], ["--top", "special"])
        self.assertNotIn("--top", by_file["a.v"])
        self.write("bad_tops.txt", "only-one-word\n")
        code, _, err = self.run_tool("--tops", str(self.dir / "bad_tops.txt"), str(self.dir / "m"))
        self.assertEqual(code, 2)
        self.assertIn("expected '<file> <top>'", err)


class ModeTest(SweepTestCase):
    def test_bench_starts_the_bench_with_one_per_file(self) -> None:
        self.write("m/a.v", "xx")
        self.write("m/b.v", "x")
        code, out, err = self.run_tool("--bench", str(self.bench), str(self.dir / "m"))
        self.assertEqual(code, 0, err)
        self.assertEqual(
            self.calls(),
            [
                ["bench", "--one", str(self.dir / "m/b.v")],
                ["bench", "--one", str(self.dir / "m/a.v")],
            ],
        )
        self.assertIn("bench: bytes=1 parse_ms=1", out)
        self.assertNotIn("--elab-only", self.log.read_text())

    def test_equiv_runs_the_bridge_and_maps_its_exit_codes(self) -> None:
        self.write("m/eq.v", "x\n")
        self.write("m/diff.v", "EQ1\n")
        self.write("m/err.v", "EQ2\n")
        self.write("m/crash.v", "EQ3\n")
        base = ["--equiv", "--yosys", "Y", "--equiv-check", "E", str(self.dir / "m")]
        code, out, _ = self.run_tool(*base)
        self.assertEqual(code, 1)
        self.assertIn("FAIL  diff.v", out)
        self.assertIn("NOT equivalent", out)
        self.assertIn("FAIL  err.v", out)
        self.assertIn("crash crash.v", out)
        self.assertIn("oracle crash", out)
        self.assertIn("1 oracle crashes", out)
        self.assertIn("1 passed", out)
        argv = next(c for c in self.calls() if c[-1].endswith("eq.v"))
        self.assertIn("--mem-cap", argv)
        self.assertEqual(argv[argv.index("--mem-cap") + 1], str(4 * GIB))
        self.assertEqual(argv[argv.index("--yosys") + 1].split("/")[-1], "Y")

    def test_equiv_forwards_the_techlib(self) -> None:
        self.write("m/eq.v", "x\n")
        base = ["--equiv", "--yosys", "Y", "--equiv-check", "E", "--techlib", "/lib/t.o3lib"]
        code, out, _ = self.run_tool(*base, str(self.dir / "m"))
        self.assertEqual(code, 0, out)
        argv = next(c for c in self.calls() if c[-1].endswith("eq.v"))
        self.assertEqual(argv[argv.index("--techlib") + 1], "/lib/t.o3lib")

    def test_a_cap_killed_bridge_run_is_never_an_expected_failure(self) -> None:
        self.write("m/oom.v", "EQOOM\n")
        listed = self.expect("oom.v [equiv-check failed] listed\n")
        code, out, _ = self.run_tool("--equiv", "--yosys", "Y", "--equiv-check", "E",
                                     "--expect-fail", listed, str(self.dir / "m"))
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL  oom.v", out)
        self.assertNotIn("xfail", out)

    def test_an_oracle_crash_alone_is_not_a_failure_and_a_listed_one_is_expected(self) -> None:
        self.write("m/crash.v", "EQ3\n")
        base = ["--equiv", "--yosys", "Y", "--equiv-check", "E"]
        code, out, _ = self.run_tool(*base, str(self.dir / "m"))
        self.assertEqual(code, 0, out)
        listed = self.expect("crash.v Yosys 0.55 segfaults\n")
        code, out, _ = self.run_tool(*base, "--expect-fail", listed, str(self.dir / "m"))
        self.assertEqual(code, 0, out)
        self.assertIn("xfail crash.v", out)

    def test_equiv_needs_its_tools_and_excludes_bench(self) -> None:
        self.write("m/a.v")
        code, _, err = self.run_tool("--equiv", str(self.dir / "m"))
        self.assertEqual(code, 2)
        self.assertIn("--equiv needs --yosys and --equiv-check", err)
        code, _, err = self.run_tool("--equiv", "--bench", str(self.bench), "--yosys", "Y",
                                     "--equiv-check", "E", str(self.dir / "m"))
        self.assertEqual(code, 2)
        self.assertIn("cannot be combined", err)

    def test_the_bridge_skipping_with_77_skips_the_run(self) -> None:
        self.write("m/a.v", "EQ77\n")
        code, _, _ = self.run_tool("--equiv", "--yosys", "Y", "--equiv-check", "E",
                                   str(self.dir / "m"))
        self.assertEqual(code, 77)


class OptModesTest(SweepTestCase):
    """2F Task 17: --opt-equiv and --opt-diff run their tool per file."""

    def tool_calls(self) -> list[list[str]]:
        return [c[1:] for c in self.calls() if c[0] == "opt-tool"]

    def test_both_modes_pass_the_options_on_and_map_the_exit_codes(self) -> None:
        fake = _exe(self.bin / "fake_opt_tool.py", FAKE_OPT_TOOL)
        self.write("m/ok.v", "x\n")
        self.write("m/diff.v", "OT1\n")
        self.write("m/err.v", "OT2\n")
        tops = self.write("tops.txt", "ok.v cpu\n")
        for mode, name in (("--opt-equiv", "opt-equiv"), ("--opt-diff", "opt-diff")):
            self.log.write_text("", encoding="utf-8")
            tools = dict(elab_sweep.OPT_TOOLS, **{name: fake})
            with mock.patch.object(elab_sweep, "OPT_TOOLS", tools):
                code, out, _ = self.run_tool(mode, "--yosys", "Y", "--opt-arg=--script=phase2",
                                             "--tops", str(tops), str(self.dir / "m"))
            self.assertEqual(code, 1)
            self.assertIn("FAIL  diff.v", out)
            self.assertIn("NOT ok: opt-tool: message for exit 1", out)
            self.assertIn("FAIL  err.v", out)
            self.assertIn("1 passed", out)
            argv = next(c for c in self.tool_calls() if c[-1].endswith("ok.v"))[1:]
            self.assertEqual(argv[argv.index("--top") + 1], "cpu")
            self.assertEqual(argv[argv.index("--yosys") + 1].split("/")[-1], "Y")
            self.assertIn("--script=phase2", argv)

    def test_modes_need_yosys_and_exclude_each_other(self) -> None:
        self.write("m/a.v", "x\n")
        self.assertEqual(self.run_tool("--opt-diff", str(self.dir / "m"))[0], 2)
        code, _, err = self.run_tool("--opt-diff", "--opt-equiv", "--yosys", "Y",
                                     str(self.dir / "m"))
        self.assertEqual(code, 2)
        self.assertIn("cannot be combined", err)


    def test_opt_run_needs_no_yosys_and_reports_the_largest_iteration_count(self) -> None:
        fake = _exe(self.bin / "fake_opt_run.py", FAKE_OPT_RUN)
        self.write("m/a.v", "IT3\n")
        self.write("m/bb.v", "IT12 and padding\n")
        self.write("m/c.v", "IT5 more padding to sort it last\n" * 4)
        self.write("m/bad.v", "BAD\n")
        tools = dict(elab_sweep.OPT_TOOLS, **{"opt-run": fake})
        with mock.patch.object(elab_sweep, "OPT_TOOLS", tools):
            code, out, _ = self.run_tool("--opt-run", "--techlib", "L.o3lib", str(self.dir / "m"))
        self.assertEqual(code, 1)
        self.assertIn("FAIL  bad.v", out)
        self.assertIn("NOT ok: opt-run: FAIL: no fixed point after 3 iterations", out)
        self.assertIn("opt-run: maximum opt iterations 12 (bb.v; the cap is 10000)", out)
        argv = next(c for c in self.tool_calls() if c[-1].endswith("a.v"))[1:]
        self.assertNotIn("--yosys", argv)
        self.assertEqual(argv[argv.index("--techlib") + 1].split("/")[-1], "L.o3lib")

    def test_an_opt_run_crash_is_hard_even_with_an_expect_fail_needle(self) -> None:
        fake = _exe(self.bin / "fake_opt_run.py", FAKE_OPT_RUN)
        self.write("m/c.v", "CRASH IT1\n")
        expect = self.expect("c.v [CRASH] listed\n")
        tools = dict(elab_sweep.OPT_TOOLS, **{"opt-run": fake})
        with mock.patch.object(elab_sweep, "OPT_TOOLS", tools):
            code, out, _ = self.run_tool("--opt-run", "--expect-fail", str(expect),
                                         str(self.dir / "m"))
        self.assertEqual(code, 1)
        self.assertIn("FAIL  c.v", out)

    def test_opt_run_excludes_the_other_modes(self) -> None:
        self.write("m/a.v", "x\n")
        code, _, err = self.run_tool("--opt-run", "--opt-diff", "--yosys", "Y",
                                     str(self.dir / "m"))
        self.assertEqual(code, 2)
        self.assertIn("cannot be combined", err)


class ProcModesTest(SweepTestCase):
    """2E Task 12: --proc-diff, and the bridge with --pass proc."""

    def proc_diff_calls(self) -> list[list[str]]:
        return [c[1:] for c in self.calls() if c[0] == "proc-diff"]

    def test_proc_diff_maps_its_exit_codes_and_passes_the_options_on(self) -> None:
        self.write("m/ok.v", "x\n")
        self.write("m/diff.v", "PD1\n")
        self.write("m/err.v", "PD2\n")
        self.write("m/tie.v", "PD3\n")
        tops = self.write("tops.txt", "ok.v cpu\n")
        code, out, _ = self.run_tool("--proc-diff", "--yosys", "Y", "--techlib", "L.o3lib",
                                     "--nolatches", "--tops", str(tops), str(self.dir / "m"))
        self.assertEqual(code, 1)
        self.assertIn("FAIL  diff.v", out)
        self.assertIn("NOT identical", out)
        self.assertIn("FAIL  err.v", out)
        self.assertIn("FAIL  tie.v", out)
        self.assertIn("tie: netlist-compare cannot order", out)
        self.assertIn("1 passed", out)
        argv = next(c for c in self.proc_diff_calls() if c[-1].endswith("ok.v"))
        self.assertEqual(argv[argv.index("--top") + 1], "cpu")
        self.assertEqual(argv[argv.index("--mem-cap") + 1], str(4 * GIB))
        self.assertEqual(Path(argv[argv.index("--techlib") + 1]).name, "L.o3lib")
        self.assertIn("--nolatches", argv)

    def test_a_listed_failure_needs_its_needle_in_proc_diffs_output(self) -> None:
        self.write("m/err.v", "PD2\n")
        base = ["--proc-diff", "--yosys", "Y", "--trio", "off", str(self.dir / "m")]
        listed = self.expect("err.v [no latch allowed] E2: listed\n")
        code, out, _ = self.run_tool(*base[:3], "--expect-fail", listed, *base[3:])
        self.assertEqual(code, 0, out)
        self.assertIn("xfail err.v", out)
        wrong = self.expect("err.v [E1a] listed\n")
        code, out, _ = self.run_tool(*base[:3], "--expect-fail", wrong, *base[3:])
        self.assertEqual(code, 1)
        self.assertIn("fails differently from the listing", out)

    def test_trio_auto_follows_opt(self) -> None:
        self.write("m/a.v", "x\n")
        base = ["--proc-diff", "--yosys", "Y", str(self.dir / "m")]
        code, out, _ = self.run_tool(*base)  # the fake odin3 cannot run the probe: no opt yet
        self.assertEqual(code, 0, out)
        self.assertIn("trio: skipped", out)
        self.assertIn("--no-trio", self.proc_diff_calls()[0])
        self.log.write_text("", encoding="utf-8")
        argv = ["--odin3", str(self.odin3_opt), "--golden-root", str(self.golden), *base]
        code, out, _ = helpers.run_main(elab_sweep.main, argv)
        self.assertEqual(code, 0, out)
        self.assertNotIn("trio: skipped", out)
        self.assertEqual(self.calls()[0], ["odin3", "-p", "opt -nodffe -nosdff"])
        self.assertNotIn("--no-trio", self.proc_diff_calls()[0])

    def test_trio_on_requires_opt_and_also_no_trio_runs_twice(self) -> None:
        self.write("m/a.v", "x\n")
        base = ["--proc-diff", "--yosys", "Y", "--trio", "on", str(self.dir / "m")]
        code, _, err = self.run_tool(*base)
        self.assertEqual(code, 2)
        self.assertIn("--trio on", err)
        argv = ["--odin3", str(self.odin3_opt), "--golden-root", str(self.golden),
                "--also-no-trio", *base]
        code, out, _ = helpers.run_main(elab_sweep.main, argv)
        self.assertEqual(code, 0, out)
        runs = self.proc_diff_calls()
        self.assertEqual(["--no-trio" in r for r in runs], [False, True])

    def test_the_bridge_ends_after_proc_with_pass_proc(self) -> None:
        self.write("m/a.v", "x\n")
        base = ["--equiv", "--yosys", "Y", "--equiv-check", "E", "--pass", "proc", "--trio", "off",
                str(self.dir / "m")]
        code, out, _ = self.run_tool(*base)
        self.assertEqual(code, 0, out)
        argv = next(c for c in self.calls() if c[0] == "elab-equiv")
        self.assertIn("proc", argv)
        self.assertEqual(argv[argv.index("--pass") + 1], "proc")
        self.assertIn("--async2sync", argv)
        self.assertIn("--unmap-enables", argv)
        self.assertIn("--no-trio", argv)  # --trio off

    def test_pass_proc_needs_the_bridge_and_proc_diff_needs_yosys(self) -> None:
        self.write("m/a.v", "x\n")
        code, _, err = self.run_tool("--pass", "proc", str(self.dir / "m"))
        self.assertEqual(code, 2)
        self.assertIn("--pass proc needs --equiv", err)
        code, _, err = self.run_tool("--proc-diff", str(self.dir / "m"))
        self.assertEqual(code, 2)
        self.assertIn("--proc-diff needs --yosys", err)


if __name__ == "__main__":
    unittest.main()


FAKE_BENCH_FULL = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["bench", *argv]) + "\\n")
text = open(argv[1]).read()
ms = "900" if "SLOW" in text else "1"
print(" ".join(f"{k}={ms if k == 'elab_ms' else 1}" for k in bench_report.ELAB_COLUMNS))
"""


class BenchReportTest(SweepTestCase):
    """Task 27: elab-sweep --bench passes --techlib on, and --bench-report prints the targets."""

    def setUp(self) -> None:
        super().setUp()
        self.bench = _exe(self.bin / "bench_elab", FAKE_BENCH_FULL.replace(
            "bench_report.ELAB_COLUMNS", repr(bench_report.ELAB_COLUMNS)))

    def test_bench_passes_the_techlib_to_each_child(self) -> None:
        self.write("m/a.v", "xx")
        lib = self.write("lib.o3lib", "x")
        code, _, err = self.run_tool("--bench", str(self.bench), "--techlib", str(lib),
                                     str(self.dir / "m"))
        self.assertEqual(code, 0, err)
        self.assertEqual(self.calls(),
                         [["bench", "--one", str(self.dir / "m/a.v"), "--techlib", str(lib)]])

    def test_bench_report_prints_the_targets_after_the_sweep(self) -> None:
        self.write("m/mcml.v", "module m; endmodule\n")
        code, out, err = self.run_tool("--bench", str(self.bench), "--bench-report", "elab",
                                       "--root", str(self.dir / "m"), str(self.dir / "m"))
        self.assertEqual(code, 0, err)
        self.assertIn("bench targets (spec 15):", out)
        self.assertRegex(out, r"ok +mcml\.v stage 1 \+ stage 2")
        self.assertRegex(out, r"n/a +lenet\.v peak RSS")

    def test_a_missed_target_fails_the_run(self) -> None:
        self.write("m/mcml.v", "SLOW\n")
        code, out, _ = self.run_tool("--bench", str(self.bench), "--bench-report", "elab",
                                     "--root", str(self.dir / "m"), str(self.dir / "m"))
        self.assertEqual(code, 1)
        self.assertRegex(out, r"MISSED +mcml\.v stage 1 \+ stage 2")
        self.assertIn("estimate terms of mcml.v", out)

    def test_bench_report_needs_bench(self) -> None:
        self.write("m/a.v")
        code, _, err = self.run_tool("--bench-report", "elab", str(self.dir / "m"))
        self.assertEqual(code, 2)
        self.assertIn("--bench-report needs --bench", err)

    def test_unknown_profile_is_a_usage_error(self) -> None:
        self.write("m/a.v")
        code, _, err = self.run_tool("--bench", str(self.bench), "--bench-report", "nope",
                                     str(self.dir / "m"))
        self.assertEqual(code, 2)
        self.assertIn("unknown bench profile", err)

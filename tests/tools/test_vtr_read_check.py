"""Tests for tools/vtr-read-check (3E spec §16.4, plan Task 11).

Every case runs fake odin3, VPR and blif-vtr-check scripts under /tmp/claude-1000, driven by
marker lines in the BLIF files:

- ``# fake-refuse: <text>``: odin3 refuses with ``write_blif: --vtr: <loc>: <text>`` (exit 1);
- ``# fake-vpr: <message>``: VPR rejects the file (exit 1, ``Message: <message>`` on stdout);
- ``# fake-checker: reject``: the checker rejects it (exit 1);
- ``# fake-warn: <text>``: odin3 warns ``write_blif: --vtr: <text>`` and writes W;
- ``# W-<marker>``: odin3 writes ``# <marker>`` into W (and drops the G-only markers).

The heavy-run cases reach qor_heavy through its seams (mock.patch.object), never the real lock,
cgroups or tools/heavy-run.
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import qor_heavy
import vtr_read_check
from vtr_read_check import ReadCtx, pairing_ok

FAKE_ODIN3 = """\
import re, sys
script = sys.argv[2]
g = re.search(r"read_blif (\\S+);", script).group(1)
w = re.search(r"write_blif --vtr (\\S+)$", script).group(1)
lines = open(g).read().splitlines()
refusals = [ln.split(":", 1)[1].strip() for ln in lines if ln.startswith("# fake-refuse:")]
if refusals:
    for text in refusals:
        print(f"odin3: error: write_blif: --vtr: {g}:1: {text}", file=sys.stderr)
    sys.exit(1)
for ln in lines:
    if ln.startswith("# fake-warn:"):
        print("odin3: warning: write_blif: --vtr: " + ln.split(":", 1)[1].strip(), file=sys.stderr)
out = [ln.replace("# W-", "# ", 1) for ln in lines if not ln.startswith("# fake-")]
open(w, "w").write("\\n".join(out) + "\\n")
"""
FAKE_VPR = """\
import sys
blif = sys.argv[2]
for line in open(blif).read().splitlines():
    if line.startswith("# fake-vpr:"):
        print("Error 1:\\nType: Blif file\\nMessage: " + line.split(":", 1)[1].strip())
        sys.exit(1)
print("VPR succeeded")
"""
FAKE_CHECKER = """\
import sys
text = open(sys.argv[-1]).read()
sys.exit(1 if "# fake-checker: reject" in text else 0)
"""
BODY = ".model top\n.inputs a\n.outputs y\n.names a y\n1 1\n.end\n"
HEADER = ("golden\ttool\twritten|refused\terror class\tvpr_W\tvpr_G\tchecker_W\tchecker_G\t"
          "verdict")
SUBCKT_DFFSR = ("Subckt instantiates model '$_DFFSR_PPP_', but no such model exists in the "
                "architecture file.")
CAP_6G = "6442450944"
NOT_CONTROLLER = "vtr-read-check: heavy runs are the controller's: set ODIN3_HEAVY_OK=1\n"


class ReadCheckBase(unittest.TestCase):
    def setUp(self) -> None:
        qor_heavy.HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=qor_heavy.HEAVY_DIR, prefix="vrc-test-")
        self.tmp = Path(self._tmp.name)
        self.vtr = self.tmp / "vtr"
        self.vpr = self.script(self.vtr / "vpr" / "vpr", FAKE_VPR)
        self.arch = self.vtr / vtr_read_check.ARCH_REL
        self.arch.parent.mkdir(parents=True)
        self.arch.write_text("<architecture/>\n")
        self.odin3 = self.script(self.tmp / "odin3", FAKE_ODIN3)
        self.checker = self.script(self.tmp / "checker", FAKE_CHECKER)
        self.golden = self.tmp / "golden"
        self.work = self.tmp / "work"
        self.work.mkdir()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def script(self, path: Path, body: str) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"#!{sys.executable} -I\n{body}")
        path.chmod(0o755)
        return path

    def golden_file(self, rel: str, *markers: str, status: str = "ok", pad: int = 0) -> Path:
        path = self.golden / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("".join(f"{m}\n" for m in markers) + BODY + "#" * pad + "\n")
        path.with_suffix(".prov").write_text(f"tool=x\nstatus={status}\n")
        return path

    def ctx(self) -> ReadCtx:
        return ReadCtx(golden_root=self.golden, arch=self.arch, vpr=self.vpr, odin3=self.odin3,
                       checker=self.checker, work=self.work)

    def run_main(self, *argv: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        env = {"VTR_ROOT": str(self.vtr)}
        with mock.patch.object(qor_heavy, "reexec_under_lock") as reexec, \
                mock.patch.dict(os.environ, env), contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(err):
            code = vtr_read_check.main([
                "--golden-root", str(self.golden), "--odin3", str(self.odin3),
                "--checker", str(self.checker), "--work", str(self.work), *argv])
        reexec.assert_called_once()
        return code, out.getvalue(), err.getvalue()


class PairingTest(unittest.TestCase):
    def test_table(self) -> None:
        self.assertTrue(pairing_ok("E1", "Only rising edge latches supported"))
        self.assertTrue(pairing_ok("E1b", SUBCKT_DFFSR))  # plan point (e)
        self.assertTrue(pairing_ok("E5", SUBCKT_DFFSR.replace("$_DFFSR_PPP_", "$pow")))
        self.assertTrue(pairing_ok(
            "E7", "BLIF .names input size (7) greater than .names model input size (6)"))
        self.assertTrue(pairing_ok("E12", "Inconsistent block data sizes"))
        self.assertTrue(pairing_ok("E3b", ""))  # accepted by VPR: an intended refusal
        self.assertTrue(pairing_ok("E8", ""))
        self.assertTrue(pairing_ok("E11", "anything"))
        self.assertTrue(pairing_ok("E11", ""))
        self.assertFalse(pairing_ok("E1", ""))
        self.assertFalse(pairing_ok("E1", SUBCKT_DFFSR))
        self.assertFalse(pairing_ok("E1b", "Only rising edge latches supported"))
        self.assertFalse(pairing_ok("E3b", "Latch must have a clock"))
        self.assertFalse(pairing_ok("?", "Only rising edge latches supported"))

    def test_e4_never_pairs(self) -> None:
        messages = ["", "Only rising edge latches supported", SUBCKT_DFFSR,
                    "Latch must have a clock", "Must be no existing net driver", "x"]
        for message in messages:
            with self.subTest(message=message):
                self.assertFalse(pairing_ok("E4", message))
                self.assertFalse(pairing_ok("E9", message))


class CollectAndRunTest(ReadCheckBase):
    def test_smallest_first(self) -> None:
        big = self.golden_file("b/big.parmys.blif", pad=300)
        small = self.golden_file("c/small.odin.blif")
        mid = self.golden_file("a/mid.parmys.blif", pad=100)
        self.golden_file("d/failed.parmys.blif", status="failed")
        (self.golden / "e").mkdir()
        (self.golden / "e" / "noprov.blif").write_text(BODY)
        self.assertEqual([small, mid, big], vtr_read_check.collect(self.golden))

    def test_written_and_refused_pass(self) -> None:
        ctx = self.ctx()
        row = vtr_read_check.run_one(self.golden_file("ok.parmys.blif"), ctx)
        self.assertEqual(("written", "0", "0", "0", "0", "pass"),
                         (row.outcome, row.vpr_w, row.vpr_g, row.checker_w, row.checker_g,
                          row.verdict))
        refused = self.golden_file(
            "dffsre.parmys.blif",
            "# fake-refuse: $_DFFSR_PPP_ '$c14': asynchronous set/reset flip-flop; VPR has none",
            f"# fake-vpr: {SUBCKT_DFFSR}", "# fake-checker: reject")
        row = vtr_read_check.run_one(refused, ctx)
        self.assertEqual(("refused", "E1b", "-", "1", "1", "pass"),
                         (row.outcome, row.classes, row.vpr_w, row.vpr_g, row.checker_g,
                          row.verdict))
        self.assertEqual([], list(self.work.iterdir()))  # passing run directories removed

    def test_written_w_vpr_rejects_fails(self) -> None:
        g = self.golden_file("bad.odin.blif", "# W-fake-vpr: Latch must have a clock")
        row = vtr_read_check.run_one(g, self.ctx())
        self.assertEqual(("written", "1", "0", "fail"),
                         (row.outcome, row.vpr_w, row.vpr_g, row.verdict))
        self.assertIn("VPR rejects W: Latch must have a clock", row.why)
        self.assertTrue((self.work / "1" / "W.blif").is_file())  # a failing directory is kept

    def test_checker_disagrees_with_vpr_on_g_fails(self) -> None:
        g = self.golden_file("odd.parmys.blif", "# fake-checker: reject")
        row = vtr_read_check.run_one(g, self.ctx())
        self.assertEqual(("written", "0", "1", "fail"),
                         (row.outcome, row.vpr_g, row.checker_g, row.verdict))
        refused = self.golden_file("fe.odin.blif", "# fake-refuse: $_DFF_N_ 'c': "
                                   "falling-edge flip-flop; VPR accepts only rising-edge",
                                   "# fake-vpr: Only rising edge latches supported")
        row = vtr_read_check.run_one(refused, self.ctx())
        self.assertEqual(("refused", "E1", "1", "0", "fail"),
                         (row.outcome, row.classes, row.vpr_g, row.checker_g, row.verdict))

    def test_unpaired_refusal_fails(self) -> None:
        g = self.golden_file("tri.parmys.blif", "# fake-refuse: $_TBUF_ 't': tri-state cell; x")
        row = vtr_read_check.run_one(g, self.ctx())
        self.assertEqual(("refused", "E4", "fail"), (row.outcome, row.classes, row.verdict))

    def test_report_header_and_exit(self) -> None:
        self.golden_file("a.parmys.blif", "# fake-warn: g:3: $_DFF_P_ 'q': clock is 'unconn'; "
                         "VPR will never clock it")
        code, out, _ = self.run_main()
        self.assertEqual(0, code, out)
        report = (self.work / "report.tsv").read_text().splitlines()
        self.assertEqual(HEADER, report[0])
        self.assertEqual("a.parmys.blif\tparmys\twritten\t-\t0\t0\t0\t0\tpass", report[1])
        self.assertIn("RESULT: PASS", out)
        self.assertIn("vtr-read-check: clock on 'unconn' (class U): a.parmys.blif\n", out)
        self.golden_file("b.odin.blif", "# W-fake-checker: reject", pad=50)
        code, out, _ = self.run_main("--report", str(self.tmp / "r.tsv"))
        self.assertEqual(1, code)
        self.assertEqual(HEADER, (self.tmp / "r.tsv").read_text().splitlines()[0])
        self.assertIn("FAIL b.odin.blif", out)

    def test_bad_corpus(self) -> None:
        corpus = self.tmp / "bad"
        corpus.mkdir()
        (corpus / "c6_fe.blif").write_text(
            "# vpr: Only rising edge\n# fake-vpr: Only rising edge latches supported\n"
            "# fake-checker: reject\n" + BODY)
        (corpus / "strict").mkdir()  # not a *.blif: skipped
        code, out, _ = self.run_main("--bad-corpus", str(corpus))
        self.assertEqual(0, code, out)
        (corpus / "c9_lenient.blif").write_text("# vpr: Must be\n# fake-checker: reject\n" + BODY)
        code, out, _ = self.run_main("--bad-corpus", str(corpus))
        self.assertEqual(1, code)
        self.assertIn("FAIL c9_lenient.blif", out)
        self.assertIn("ok   c6_fe.blif", out)


class HeavyTest(ReadCheckBase):
    def seams(self, cgroup: str, *, fd9: bool) -> contextlib.ExitStack:
        self.lock = self.tmp / "odin3-heavy.lock"
        self.lock.touch()
        self_cgroup = self.tmp / "self-cgroup"
        self_cgroup.write_text(f"0::/{cgroup}\n")
        link = self.tmp / "fd9"
        link.unlink(missing_ok=True)
        if fd9:
            link.symlink_to(self.lock)
        stack = contextlib.ExitStack()
        for name, value in (("SELF_CGROUP", self_cgroup), ("CGROUP_ROOT", self.tmp / "cg"),
                            ("FD9", link), ("HEAVY_LOCK", self.lock),
                            ("HEAVY_RUN", self.tmp / "heavy-run")):
            stack.enter_context(mock.patch.object(qor_heavy, name, value))
        return stack

    def test_reexec_through_heavy_run(self) -> None:
        record = self.tmp / "record.json"
        self.script(self.tmp / "heavy-run",
                    "import json, os, sys\n"
                    f"json.dump({{'argv': sys.argv, 'env': dict(os.environ)}}, "
                    f"open({str(record)!r}, 'w'))\nsys.exit(0)\n")
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C.UTF-8",
               "ODIN3_HEAVY_OK": "1",
               "VRC_TEST_MARKER": "caller"}
        with self.seams("user.slice/x.scope", fd9=False), \
                mock.patch.dict(os.environ, env, clear=True), \
                self.assertRaises(SystemExit) as ctx:
            vtr_read_check.main(["--limit", "3"])
        self.assertEqual(0, ctx.exception.code)
        seen = json.loads(record.read_text())
        self.assertEqual([str(self.tmp / "heavy-run"), sys.executable, "-I",
                          str(vtr_read_check.SCRIPT), "--limit", "3"], seen["argv"])
        self.assertEqual(env, seen["env"])
        script = qor_heavy.REPO_ROOT / "tools" / "vtr-read-check" / "vtr_read_check.py"
        self.assertEqual(script, vtr_read_check.SCRIPT)
        self.assertEqual(str(qor_heavy.REPO_ROOT / "tools" / "heavy-run"),
                         qor_heavy.reexec_argv([], script)[0])

        record.unlink()
        err = io.StringIO()
        with self.seams("user.slice/x.scope", fd9=False), \
                mock.patch.dict(os.environ, {"PATH": env["PATH"]}, clear=True), \
                contextlib.redirect_stderr(err):
            self.assertEqual(2, vtr_read_check.main(["--limit", "3"]))
        self.assertEqual(NOT_CONTROLLER, err.getvalue())
        self.assertFalse(record.exists())

    def test_inside_with_failing_cap_exits_2(self) -> None:
        slice_dir = self.tmp / "cg" / "user.slice" / "odin3-heavy.slice"
        scope = slice_dir / "odin3-heavy-1.scope"
        scope.mkdir(parents=True)
        for directory in (slice_dir, scope):  # no cap anywhere: verify_scope fails
            (directory / "memory.max").write_text("max\n")
            (directory / "memory.swap.max").write_text("max\n")
        err = io.StringIO()
        env = {"ODIN3_HEAVY_SLICE": "odin3-heavy.slice", "VTR_ROOT": str(self.vtr)}
        with self.seams(str(scope.relative_to(self.tmp / "cg")), fd9=True), \
                mock.patch.dict(os.environ, env), contextlib.redirect_stderr(err):
            self.assertEqual(2, vtr_read_check.main(["--golden-root", str(self.golden)]))
        self.assertIn("vtr-read-check: not inside a capped scope", err.getvalue())
        self.assertFalse((self.tmp / "heavy-run").exists())

        (slice_dir / "memory.max").write_text(CAP_6G + "\n")  # the cap holds: it runs
        (slice_dir / "memory.swap.max").write_text("0\n")
        self.golden_file("a.parmys.blif")
        out = io.StringIO()
        with self.seams(str(scope.relative_to(self.tmp / "cg")), fd9=True), \
                mock.patch.dict(os.environ, env), contextlib.redirect_stdout(out):
            code = vtr_read_check.main([
                "--golden-root", str(self.golden), "--odin3", str(self.odin3),
                "--checker", str(self.checker), "--work", str(self.work)])
        self.assertEqual(0, code, out.getvalue())


if __name__ == "__main__":
    unittest.main()

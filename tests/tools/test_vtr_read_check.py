"""Tests for tools/vtr-read-check (3E plan Task 11, spec 16.4).

Fake ``odin3``, ``vpr`` and ``blif-vtr-check`` scripts, driven by ``# fake:`` lines in the files
they are given, stand for the real tools; the heavy-run gate is tested through the seams of
``qor_heavy`` (a fake ``heavy-run``, a fake cgroup tree), as ``test_vtr_qor_heavy`` does. Nothing
touches the real heavy lock.
"""

from __future__ import annotations

import contextlib
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import qor_heavy
import vtr_read_check as vrc

from . import helpers

SCRATCH = Path("/tmp/claude-1000")
HEADER = (
    "golden\ttool\twritten|refused\terror class\tvpr_W\tvpr_G\tchecker_W\tchecker_G\tverdict"
)
SLICE = "odin3-heavy.slice"
SCOPE = "odin3-heavy-4242-1760000000000000000.scope"

# `odin3 -p "read_techlib L; read_blif G; write_blif --vtr W"`. G's `# fake:` lines: `refuse E1|E3b|
# E7|E12` (log an error line of that class, exit 1), `readrefuse` (read_blif's multi-driver
# refusal, exit 1), `crash` (exit 3, no error line), `warn` (an
# info line). The default copies G to W.
FAKE_ODIN3 = r"""#!/usr/bin/env python3
import os, re, sys
log = os.environ["FAKE_LOG"]
script = sys.argv[sys.argv.index("-p") + 1]
with open(log, "a") as fh:
    fh.write("odin3 " + script + "\n")
g = re.search(r'read_blif "([^"]*)"', script)[1]
w = re.search(r'write_blif --vtr "([^"]*)"', script)[1]
text = open(g).read()
facts = [l[8:].split() for l in text.splitlines() if l.startswith("# fake: ")]
MSG = {
    "E1": "x.blif:1: $_DFF_N_ '$c1': falling-edge flip-flop; VPR accepts only rising-edge latches",
    "E1b": "x.blif:1: $_DFFSR_PPP_ '$c1': asynchronous set/reset flip-flop; VPR has none",
    "E3b": "x.blif:1: single_port_ram '$c1': clock input 'clk' is unconnected or x",
    "E4": "x.blif:1: $tribuf '$c1': tri-state cell; VPR has no tri-state",
    "E7": "x.blif:1: $sop '$c1': 7 inputs exceed K=6 (run abc, or pass --lut-k)",
    "E12": "m: net 'y' has 2 drivers ('$c4', '$c5')",
}
for f in facts:
    if f[0] == "crash":
        print("odin3: error: read_blif: boom", file=sys.stderr)
        sys.exit(3)
    if f[0] == "readrefuse":  # read_blif's own refusal of a multiply-driven net (PHASE3 #36)
        print("odin3: error: " + g + ":8: net 'z' is driven by 2 cell outputs (one allowed)",
              file=sys.stderr)
        print("odin3: error: pass read_blif: failed: ODIN3_ERR_PARSE", file=sys.stderr)
        sys.exit(1)
    if f[0] == "refuse":
        print("odin3: error: write_blif: --vtr: " + MSG[f[1]], file=sys.stderr)
        sys.exit(1)
    if f[0] == "warn":
        print("odin3: warning: write_blif: --vtr: net 'unconn' feeds 2 .names/.latch pins",
              file=sys.stderr)
open(w, "w").write(text)
"""

# `vpr ARCH CIRCUIT --circuit_format blif --exit_before_pack on`; the circuit's `# vpr: ` lines:
# `reject <message>` (stdout `Message: …`, exit 1), `assert` (stderr assertion, exit 134).
FAKE_VPR = r"""#!/usr/bin/env python3
import os, sys
with open(os.environ["FAKE_LOG"], "a") as fh:
    fh.write("vpr " + " ".join(sys.argv[1:]) + " (cwd " + os.path.basename(os.getcwd()) + ")\n")
text = open(sys.argv[2]).read()
for line in text.splitlines():
    if line.startswith("# vpr-fake: reject "):
        print("Error 1: \nType: Blif file\nMessage: " + line[len("# vpr-fake: reject "):])
        sys.exit(1)
    if line.startswith("# vpr-fake: kill"):
        if line.strip() == "# vpr-fake: kill":  # the cap kills it: the cgroup counts it
            with open(os.environ["FAKE_EVENTS"], "a") as ev:
                ev.write("x")
        os.kill(os.getpid(), 9)
    if line.startswith("# vpr-fake: assert"):
        print("/x/netlist.tpp:1951 f: Assertion 'a == b' failed (Must be no existing net driver).",
              file=sys.stderr)
        sys.exit(134)
print("VPR succeeded")
"""

# `blif-vtr-check [--strict --writer-log LOG] FILE`; the file's `# chk-fake: reject` rejects, and
# `# chk-fake: strict` rejects only with --strict.
FAKE_CHECKER = r"""#!/usr/bin/env python3
import os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as fh:
    fh.write("checker " + " ".join(args) + "\n")
text = open(args[-1]).read()
if "# chk-fake: reject" in text or ("# chk-fake: strict" in text and "--strict" in args):
    print("blif-vtr-check: " + args[-1] + ":1: bad")
    sys.exit(1)
"""


def make_exec(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def netlist(*lines: str, pad: int = 0) -> str:
    return "".join(f"{line}\n" for line in lines) + ".model top\n.end\n" + "# pad\n" * pad


class ReadCheckBase(unittest.TestCase):
    def setUp(self) -> None:
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=SCRATCH, prefix="vtr-read-check-test-")
        self.tmp = Path(self._tmp.name)
        self.log = self.tmp / "calls.log"
        self.odin3 = make_exec(self.tmp / "odin3", FAKE_ODIN3)
        self.vpr = make_exec(self.tmp / "vpr", FAKE_VPR)
        self.checker = make_exec(self.tmp / "checker", FAKE_CHECKER)
        self.arch = self.tmp / "arch.xml"
        self.arch.write_text("<arch/>", encoding="utf-8")
        self.root = self.tmp / "golden"
        self.root.mkdir()
        self.work = self.tmp / "work"
        self.env = mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log)})
        self.env.start()
        self.scope_dir = self.tmp / "scope"
        self.scope_dir.mkdir()
        self.events = self.tmp / "events"
        self.events.write_text("", encoding="utf-8")
        self.env_events = mock.patch.dict(os.environ, {"FAKE_EVENTS": str(self.events)})
        self.env_events.start()
        self.scope = qor_heavy.ScopeInfo(self.scope_dir, self.scope_dir, 6 * 1024**3, True)
        self.inside = mock.patch.object(qor_heavy, "reexec_under_lock", return_value=self.scope)
        self.reexec = self.inside.start()
        self.kills = mock.patch.object(
            qor_heavy, "oom_kills", side_effect=lambda _s: len(self.events.read_text()))
        self.kills.start()

    def tearDown(self) -> None:
        self.kills.stop()
        self.env_events.stop()
        self.inside.stop()
        self.env.stop()
        self._tmp.cleanup()

    def add(self, name: str, text: str, ok: bool = True) -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        path.with_suffix(".prov").write_text("status=ok\n" if ok else "status=failed\n")
        return path

    def args(self, *extra: str) -> list[str]:
        return ["--golden-root", str(self.root), "--arch", str(self.arch), "--odin3",
                str(self.odin3), "--vpr", str(self.vpr), "--checker", str(self.checker),
                "--work", str(self.work), *extra]

    def run_main(self, *extra: str) -> tuple[int, str, str]:
        return helpers.run_main(vrc.main, self.args(*extra))

    def rows(self) -> list[list[str]]:
        lines = (self.work / "report.tsv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], HEADER)
        return [line.split("\t") for line in lines[1:]]

    def calls(self) -> list[str]:
        return self.log.read_text(encoding="utf-8").splitlines() if self.log.exists() else []


class PairingTest(unittest.TestCase):
    def test_e1_pairs_with_the_latch_message(self) -> None:
        self.assertTrue(vrc.pairing_ok("E1", "Only rising edge latches supported"))
        self.assertTrue(vrc.pairing_ok("E2", "Only rising edge latches supported"))
        self.assertFalse(vrc.pairing_ok("E1", "Latch must have a clock"))
        self.assertFalse(vrc.pairing_ok("E1", ""))

    def test_e1b_pairs_with_the_subckt_message(self) -> None:
        """Point (e): an async flip-flop reaches a golden as a `.subckt`."""
        message = ("Subckt instantiates model '$_DFFSR_PPP_', but no such model exists in the "
                   "architecture file.")
        self.assertTrue(vrc.pairing_ok("E1b", message))
        self.assertFalse(vrc.pairing_ok("E1b", "Only rising edge latches supported"))
        self.assertFalse(vrc.pairing_ok("E1b", ""))

    def test_e4_and_e9_pair_with_nothing(self) -> None:
        messages = ["", "Only rising edge latches supported", "Latch must have a clock",
                    "Subckt instantiates model 'x'", "Assertion 'x' failed", "tri-state"]
        for refusal in ("E4", "E9"):
            for message in messages:
                self.assertFalse(vrc.pairing_ok(refusal, message), (refusal, message))
        self.assertFalse(vrc.pairing_ok("E99", "anything"))

    def test_the_rest_of_the_table(self) -> None:
        cases = {
            "E3": "Latch must have a clock",
            "E5": "Subckt instantiates model '$pow', but no such model exists",
            "E6": "Blackbox BLIF model 'x' has no equivalent architecture model.",
            "E7": "BLIF .names input size (7) greater than .names model input size (6)",
            "E10": "Found multiple models with primitives. Only one model can contain primitives",
            "E12": "x.tpp:1951 f: Assertion 'a == b' failed (Must be no existing net driver).",
            "E12 ": "Inconsistent block data sizes",
            "E13": "No non-blackbox models found in the BLIF",
        }
        for refusal, message in cases.items():
            refusal = refusal.strip()
            self.assertTrue(vrc.pairing_ok(refusal, message), refusal)
            self.assertFalse(vrc.pairing_ok(refusal, ""), refusal)
            self.assertFalse(vrc.pairing_ok(refusal, "something else"), refusal)

    def test_e12_does_not_pair_with_an_unrelated_assertion(self) -> None:
        self.assertFalse(vrc.pairing_ok("E12", "x.cpp:9 f: Assertion 'clk_pin' failed"))
        r16 = "n.cpp:1 f: Assertion 'pin_net(pin_id) == net_id' failed"
        self.assertTrue(vrc.pairing_ok("E12", r16))

    def test_accepted_and_depends(self) -> None:
        for refusal in ("E3b", "E8"):  # VPR accepts G: an intended refusal
            self.assertTrue(vrc.pairing_ok(refusal, ""))
            self.assertFalse(vrc.pairing_ok(refusal, "Only rising edge latches supported"))
        self.assertTrue(vrc.pairing_ok("E11", ""))
        self.assertTrue(vrc.pairing_ok("E11", "anything"))

    def test_refusal_classes_from_the_log(self) -> None:
        log = (
            "odin3: error: write_blif: --vtr: a.blif:3: $_DFF_N_ '$c1': falling-edge "
            "flip-flop; VPR accepts only rising-edge latches\n"
            "odin3: error: write_blif: --vtr: a.blif:4: $sop '$c2': 7 inputs exceed K=6\n"
            "odin3: error: write_blif: --vtr: m: net 'y' has 2 drivers ('a', 'b')\n"
            "odin3: error: write_blif: --vtr: a.blif:5: $_DFF_N_ '$c9': falling-edge flip-flop\n"
            "odin3: error: write_blif: --vtr: 3 more errors not shown\n"
            "odin3: info: write_blif: other\n"
        )
        self.assertEqual(vrc.refusal_classes(log), ["E1", "E7", "E12"])
        self.assertEqual(vrc.refusal_classes(""), [])

    def test_read_refusal_is_e12_only_for_the_listed_goldens(self) -> None:
        log = (
            "odin3: error: /g/elsif_both_defined.odin.blif:8: net 'z' is driven by 2 cell "
            "outputs (one allowed)\n"
            "odin3: error: pass read_blif: failed: ODIN3_ERR_PARSE\n"
        )
        for name in ("elsif_both_defined.odin.blif", "multi_assignment.odin.blif"):
            self.assertEqual(vrc.refusal_classes(log, Path("/g/arch/" + name)), ["E12"])
        self.assertEqual(vrc.refusal_classes(log, Path("/g/other.odin.blif")), [])
        self.assertEqual(vrc.refusal_classes(log), [])
        other = "odin3: error: /g/multi_assignment.odin.blif:3: unknown formal 'q'\n"
        self.assertEqual(vrc.refusal_classes(other, Path("/g/multi_assignment.odin.blif")), [])


class SweepTest(ReadCheckBase):
    def test_smallest_first_and_the_steps(self) -> None:
        self.add("big/big.parmys.blif", netlist(pad=40))
        self.add("small/small.odin.blif", netlist())
        self.add("mid/mid.parmys.blif", netlist(pad=5))
        self.add("skipped/skipped.parmys.blif", netlist(), ok=False)
        code, out, err = self.run_main()
        self.assertEqual(code, 0, out + err)
        rows = self.rows()
        self.assertEqual([r[0] for r in rows],
                         ["small/small.odin.blif", "mid/mid.parmys.blif", "big/big.parmys.blif"])
        self.assertEqual(rows[0], ["small/small.odin.blif", "odin", "written", "-", "ok", "ok",
                                   "ok", "ok", "pass"])
        calls = self.calls()
        first = calls[:5]
        self.assertEqual(
            [c.split(" ")[0] for c in first], ["odin3", "vpr", "checker", "vpr", "checker"], first)
        golden = self.root / "small" / "small.odin.blif"
        self.assertIn(f'read_techlib "{vrc.TECHLIB}"; read_blif "{golden}"', first[0])
        self.assertIn('; write_blif --vtr "', first[0])
        self.assertTrue(first[1].endswith(
            "small.odin.blif --circuit_format blif --exit_before_pack on (cwd vpr_g)"), first[1])
        self.assertTrue(first[2].endswith("small.odin.blif") and "--strict" not in first[2])
        self.assertTrue(first[3].endswith("W.blif --circuit_format blif --exit_before_pack on "
                                          "(cwd vpr_w)"), first[3])
        self.assertIn(f"vpr {self.arch} ", first[3])
        self.assertTrue(first[4].startswith("checker --strict --writer-log "), first[4])
        self.assertTrue(first[4].endswith("W.blif"), first[4])
        order = [c.split('read_blif "')[1].split('"')[0].rsplit("/", 1)[1]
                 for c in calls if c.startswith("odin3")]
        self.assertEqual(order, ["small.odin.blif", "mid.parmys.blif", "big.parmys.blif"])

    def test_the_header_is_exact_and_passing_directories_are_removed(self) -> None:
        self.add("a/a.parmys.blif", netlist())
        self.assertEqual(self.run_main()[0], 0)
        self.rows()
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), ["report.tsv"])

    def test_limit(self) -> None:
        self.add("a/a.parmys.blif", netlist())
        self.add("b/b.parmys.blif", netlist(pad=3))
        self.assertEqual(self.run_main("--limit", "1")[0], 0)
        self.assertEqual([r[0] for r in self.rows()], ["a/a.parmys.blif"])

    def test_a_written_netlist_vpr_rejects_fails(self) -> None:
        self.add("a/a.parmys.blif", netlist("# vpr-fake: reject Latch must have a clock"))
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        row = self.rows()[0]
        self.assertEqual((row[2], row[4], row[8]), ("written", "fail", "fail"))
        self.assertIn("FAIL a/a.parmys.blif: VPR rejects W", out)
        self.assertTrue(any(p.is_dir() for p in self.work.iterdir()))  # kept for a look

    def test_a_checker_that_rejects_w_fails(self) -> None:
        self.add("a/a.odin.blif", netlist("# chk-fake: strict"))
        self.assertEqual(self.run_main()[0], 1)
        row = self.rows()[0]
        self.assertEqual((row[4], row[6], row[7], row[8]), ("ok", "fail", "ok", "fail"))

    def test_checker_and_vpr_disagree_on_g(self) -> None:
        self.add("a/a.parmys.blif", netlist("# vpr-fake: reject Anything"))
        # VPR rejects G (and W, a copy); the checker accepts: both a written-W failure and a
        # disagreement.
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("checker and VPR disagree on G", out)
        self.add("b/b.parmys.blif", netlist("# chk-fake: reject", pad=9))
        self.assertEqual(self.run_main()[0], 1)

    def test_a_refusal_pairs_with_vpr_on_g(self) -> None:
        self.add("fe/fe.parmys.blif", netlist(
            "# fake: refuse E1", "# vpr-fake: reject Only rising edge latches supported",
            "# chk-fake: reject"))
        code, out, err = self.run_main()
        self.assertEqual(code, 0, out + err)
        row = self.rows()[0]
        self.assertEqual(row, ["fe/fe.parmys.blif", "parmys", "refused", "E1", "-", "fail", "-",
                               "fail", "pass"])

    def test_a_refusal_that_does_not_pair_fails(self) -> None:
        self.add("fe/fe.parmys.blif", netlist(
            "# fake: refuse E1", "# vpr-fake: reject Latch must have a clock",
            "# chk-fake: reject"))
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("refusal E1 does not pair with VPR on G (Latch must have a clock)", out)
        self.assertEqual(self.rows()[0][8], "fail")

    def test_intended_refusals_are_listed(self) -> None:
        self.add("ram/ram.odin.blif", netlist("# fake: refuse E3b"))
        code, out, err = self.run_main()
        self.assertEqual(code, 0, out + err)
        self.assertEqual(self.rows()[0][8:], ["listed"])
        self.assertIn("  verdict listed: 1", out)

    def test_a_multi_driver_refusal_pairs_with_the_assertion(self) -> None:
        self.add("m/m.odin.blif", netlist("# fake: refuse E12", "# vpr-fake: assert",
                                          "# chk-fake: reject"))
        code, out, err = self.run_main()
        self.assertEqual(code, 0, out + err)
        self.assertEqual(self.rows()[0][3:6], ["E12", "-", "crash"])

    def test_a_read_refusal_of_a_listed_multi_driver_golden_is_e12_and_passes(self) -> None:
        self.add("m/multi_assignment.odin.blif", netlist("# fake: readrefuse", "# vpr-fake: assert",
                                                         "# chk-fake: reject"))
        code, out, err = self.run_main()
        self.assertEqual(code, 0, out + err)
        row = self.rows()[0]
        self.assertEqual((row[2], row[3], row[8]), ("refused", "E12", "pass"))

    def test_a_read_refusal_of_any_other_golden_still_fails(self) -> None:
        self.add("m/other.odin.blif", netlist("# fake: readrefuse", "# vpr-fake: assert",
                                              "# chk-fake: reject"))
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        row = self.rows()[0]
        self.assertEqual((row[2], row[8]), ("failed", "fail"))
        self.assertIn("odin3 failed:", out)

    def test_a_listed_step_the_cap_kills_is_capped_listed_not_pass(self) -> None:
        self.add("bgm/bgm.parmys.blif", netlist("# vpr-fake: kill"))
        code, out, err = self.run_main()
        self.assertEqual(code, 0, out + err)
        row = self.rows()[0]
        self.assertEqual((row[4], row[5], row[8]), ("oom", "oom", "capped (listed)"))
        self.assertIn("  verdict capped (listed): 1", out)
        self.assertIn("  verdict pass: 0", out)
        self.assertIn("memory cap 6 GiB", out)

    def test_an_unlisted_capped_file_fails(self) -> None:
        self.add("big/big.parmys.blif", netlist("# vpr-fake: kill"))
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("not in EXPECTED_CAPPED (big)", out)
        self.assertEqual(self.rows()[0][8], "fail")

    def test_a_listed_file_that_reads_fine_is_a_stale_entry(self) -> None:
        self.add("LargeRam/LargeRam.odin.blif", netlist())
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("listed in EXPECTED_CAPPED but read fine", out)

    def test_a_sigkill_the_cap_did_not_make_is_a_crash_not_capped(self) -> None:
        self.add("bgm/bgm.parmys.blif", netlist("# vpr-fake: kill-outside"))
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        row = self.rows()[0]
        self.assertEqual((row[5], row[8]), ("crash", "fail"))
        self.assertIn("SIGKILL without an oom_kill", out)

    def test_an_odin3_failure_fails(self) -> None:
        self.add("x/x.parmys.blif", netlist("# fake: crash"))
        code, out, _ = self.run_main()
        self.assertEqual(code, 1)
        self.assertEqual(self.rows()[0][2], "failed")
        self.assertIn("odin3 failed: odin3: error: read_blif: boom", out)

    def test_the_summary(self) -> None:
        self.add("a/a.parmys.blif", netlist())
        self.add("b/b.odin.blif", netlist("# fake: refuse E3b"))
        _, out, _ = self.run_main()
        self.assertIn("2 goldens", out)
        self.assertIn("  parmys: written: 1", out)
        self.assertIn("  odin: refused E3b: 1", out)

    def test_no_golden_is_a_usage_error(self) -> None:
        self.add("a/a.parmys.blif", netlist(), ok=False)
        self.assertEqual(self.run_main()[0], 2)

    def test_a_work_dir_outside_the_allowed_roots_is_refused(self) -> None:
        self.add("a/a.parmys.blif", netlist())
        code, _, err = helpers.run_main(vrc.main, [*self.args()[:-2], "--work", "/var/tmp/x"])
        self.assertEqual(code, 2)
        self.assertIn("not under ~/odin3-ws/work or /tmp/claude-1000", err)


class BadCorpusTest(ReadCheckBase):
    def corpus(self, **files: str) -> Path:
        corpus = self.tmp / "blif-bad"
        corpus.mkdir(exist_ok=True)
        for name, text in files.items():
            (corpus / f"{name}.blif").write_text(text, encoding="utf-8")
        return corpus

    def run_corpus(self, corpus: Path) -> tuple[int, str, str]:
        return helpers.run_main(
            vrc.main,
            ["--bad-corpus", str(corpus), "--arch", str(self.arch), "--vpr", str(self.vpr),
             "--checker", str(self.checker), "--work", str(self.work)],
        )

    def test_agreement_passes_and_needs_no_heavy_run(self) -> None:
        corpus = self.corpus(
            c1="# vpr: Only rising edge\n# vpr-fake: reject Only rising edge latches supported\n"
               "# chk-fake: reject\n.model top\n.end\n",
            c8="# vpr: Duplicate blocks named 'out:y' found in netlist.\n"
               "# vpr-fake: reject Duplicate blocks named 'out:y' found in netlist. Block 3\n"
               "# chk-fake: reject\n.model top\n.end\n",
            c9="# vpr: Must be no existing net driver\n# vpr-fake: assert\n# chk-fake: reject\n"
               ".model top\n.end\n",
        )
        (corpus / "strict").mkdir()
        strict = corpus / "strict" / "cs1.blif"
        strict.write_text("# C-S1\n# chk-fake: strict\n.model top\n.end\n", encoding="utf-8")
        code, out, err = self.run_corpus(corpus)
        self.assertEqual(code, 0, out + err)
        self.assertIn("bad corpus: all agree", out)
        self.reexec.assert_not_called()

    def test_a_relative_corpus_path_works(self) -> None:
        """Every step runs in its own directory, so the paths must be made absolute."""
        self.corpus(c1="# vpr: Only rising\n# vpr-fake: reject Only rising edge\n"
                       "# chk-fake: reject\n.model top\n.end\n")
        here = Path.cwd()
        os.chdir(self.tmp)
        try:
            code, out, err = helpers.run_main(
                vrc.main,
                ["--bad-corpus", "blif-bad", "--arch", "arch.xml", "--vpr", "vpr",
                 "--checker", "checker", "--work", str(self.work)],
            )
        finally:
            os.chdir(here)
        self.assertEqual(code, 0, out + err)

    def test_vpr_accepts_but_the_checker_rejects(self) -> None:
        corpus = self.corpus(c4="# vpr: BLIF .names input size\n# chk-fake: reject\n.model top\n"
                                ".end\n")
        code, out, _ = self.run_corpus(corpus)
        self.assertEqual(code, 1)
        self.assertIn("FAIL c4.blif: VPR accepts it, the checker fail", out)

    def test_the_checker_accepts_but_vpr_rejects(self) -> None:
        corpus = self.corpus(c4="# vpr: Latch\n# vpr-fake: reject Latch must have a clock\n"
                                ".model top\n.end\n")
        code, out, _ = self.run_corpus(corpus)
        self.assertEqual(code, 1)
        self.assertIn("VPR rejects it, the checker ok", out)

    def test_the_message_must_occur_in_vpr_output(self) -> None:
        corpus = self.corpus(c4="# vpr: Latch must have a clock\n# vpr-fake: reject Other\n"
                                "# chk-fake: reject\n.model top\n.end\n")
        code, out, _ = self.run_corpus(corpus)
        self.assertEqual(code, 1)
        self.assertIn("VPR's message lacks 'Latch must have a clock'", out)

    def test_the_assertion_text_is_searched_on_stderr(self) -> None:
        corpus = self.corpus(c9="# vpr: Must be no existing net driver\n# vpr-fake: assert\n"
                                "# chk-fake: reject\n.model top\n.end\n")
        self.assertEqual(self.run_corpus(corpus)[0], 0)


class HeavyGateTest(ReadCheckBase):
    """Plan point (a): the sweep re-runs itself through tools/heavy-run, or verifies the cap."""

    def setUp(self) -> None:
        super().setUp()
        self.inside.stop()  # the real reexec_under_lock, with qor_heavy's seams patched
        self.lock = self.tmp / "odin3-heavy.lock"
        self.lock.touch()
        self.record = self.tmp / "heavy.json"
        self.heavy_run = make_exec(
            self.tmp / "heavy-run",
            "#!/usr/bin/env python3\n"
            "import json, os, sys\n"
            f"out = open({str(self.record)!r}, 'w')\n"
            "json.dump({'argv': sys.argv, 'env': dict(os.environ)}, out)\n",
        )
        self.add("a/a.parmys.blif", netlist())

    def tearDown(self) -> None:
        self.inside.start()  # the base tearDown stops it
        super().tearDown()

    def caller_env(self, **extra: str) -> dict[str, str]:
        env = {k: v for k, v in os.environ.items() if not k.startswith("ODIN3_HEAVY")}
        env["FAKE_LOG"] = str(self.log)
        return {**env, **extra}

    def test_outside_with_the_variable_it_reruns_through_heavy_run(self) -> None:
        env = self.caller_env(ODIN3_HEAVY_OK="1")
        with mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(qor_heavy, "HEAVY_RUN", self.heavy_run), \
                mock.patch.object(qor_heavy, "HEAVY_LOCK", self.lock), \
                mock.patch.object(qor_heavy, "SELF_CGROUP", self.tmp / "no-cgroup"), \
                self.assertRaises(SystemExit) as ctx:
            vrc.main(self.args())
        self.assertEqual(ctx.exception.code, 0)
        seen = json.loads(self.record.read_text(encoding="utf-8"))
        self.assertEqual(seen["env"], env)  # the caller's environment, unchanged
        self.assertEqual(
            seen["argv"],
            [str(self.heavy_run), sys.executable, "-I", str(vrc.SCRIPT), *self.args()],
        )
        self.assertEqual(self.calls(), [])  # nothing ran outside the scope

    def test_outside_without_the_variable_it_exits_2(self) -> None:
        with mock.patch.dict(os.environ, self.caller_env(), clear=True), \
                mock.patch.object(qor_heavy, "HEAVY_RUN", self.heavy_run), \
                mock.patch.object(qor_heavy, "HEAVY_LOCK", self.lock), \
                mock.patch.object(qor_heavy, "SELF_CGROUP", self.tmp / "no-cgroup"):
            code, _, err = helpers.run_main(vrc.main, self.args())
        self.assertEqual(code, 2)
        self.assertEqual(err, "vtr-qor: heavy runs are the controller's: set ODIN3_HEAVY_OK=1\n")
        self.assertFalse(self.record.exists())
        self.assertEqual(self.calls(), [])

    def run_inside(self, memory_max: str) -> tuple[int, str, str]:
        """The sweep inside the slice: a fake cgroup tree, /proc/self/cgroup and descriptor 9."""
        root = self.tmp / "cgroup"
        scope_dir = root / "user.slice" / SLICE / SCOPE
        scope_dir.mkdir(parents=True)
        (scope_dir.parent / "memory.max").write_text(f"{memory_max}\n")
        (scope_dir.parent / "memory.swap.max").write_text("0\n")
        (scope_dir / "memory.max").write_text("max\n")
        self_cgroup = self.tmp / "self-cgroup"
        self_cgroup.write_text(f"0::/user.slice/{SLICE}/{SCOPE}\n")
        fd9 = self.tmp / "fd9"
        fd9.symlink_to(self.lock)
        env = self.caller_env(ODIN3_HEAVY_SLICE=SLICE)
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.dict(os.environ, env, clear=True))
            for name, value in (("CGROUP_ROOT", root), ("SELF_CGROUP", self_cgroup),
                                ("FD9", fd9), ("HEAVY_LOCK", self.lock),
                                ("HEAVY_RUN", self.heavy_run)):
                stack.enter_context(mock.patch.object(qor_heavy, name, value))
            return helpers.run_main(vrc.main, self.args())

    def test_inside_with_a_failing_cap_check_it_exits_2(self) -> None:
        code, _, err = self.run_inside("8589934592")  # 8G: not the 6G cap
        self.assertEqual(code, 2)
        self.assertIn("vtr-qor: not inside a capped scope (memory.max 8589934592", err)
        self.assertEqual(self.calls(), [])
        self.assertFalse(self.record.exists())

    def test_inside_with_the_cap_it_runs_the_sweep(self) -> None:
        code, out, err = self.run_inside("6442450944")
        self.assertEqual(code, 0, out + err)
        self.assertEqual(self.rows()[0][8], "pass")
        self.assertFalse(self.record.exists())  # no heavy-run: it is already inside one


if __name__ == "__main__":
    unittest.main()

"""Tests for tools/exit-check (2H; PHASE2 #4, decision 13), over fake sp-splice, netlist-compare
and equiv-check scripts and a fake golden tree (every golden with its ``.prov``)."""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from collections.abc import Sequence
from pathlib import Path
from unittest import mock

import elab_sweep
import exit_check
import memcap

from . import helpers

COMMIT = "3c9a4d23b27d187bd67c79dc01a18ff1ffc21d9b"
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
if "SPLICEHANG" in text:  # a child that outlives a kill of just this process
    import subprocess, time
    child = subprocess.Popen(["sleep", "300"])
    with open(os.environ["FAKE_PID"], "w") as handle:
        handle.write(str(child.pid))
    time.sleep(300)
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
text = open(argv[-1]).read()
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
import json, os, signal, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["equiv-check", *argv]) + "\\n")
text = open(argv[-1]).read()
if "EQERR" in text:
    print("equiv-check: x.blif: unsupported: latch type 'ah'", file=sys.stderr)
    sys.exit(2)
if "EQUNPROVEN" in text:
    print("equiv-check: unproven: ABC dsec timed out after 600 seconds", file=sys.stderr)
    sys.exit(4)
if "EQGUESS" in text:
    print("equiv-check: inconclusive: 2 unnamed black-box pair(s) were guessed", file=sys.stderr)
    sys.exit(4)
if "EQHARD" in text:
    print("equiv-check: ABC killed by signal 11", file=sys.stderr)
    sys.exit(5)
if "EQSEGV" in text:
    os.kill(os.getpid(), signal.SIGSEGV)
if "EQCRASH" in text:  # an uncaught Python exception of the old equiv-check: traceback, exit 1
    print("Traceback (most recent call last):\\nKeyError: 'x'", file=sys.stderr)
    sys.exit(1)
if "EQPRLIMIT" in text:
    print("prlimit: failed to set the AS resource limit", file=sys.stderr)
    sys.exit(1)
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


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
        self.pid_file = self.dir / "child.pid"
        self.meminfo = self.dir / "meminfo"
        self.meminfo.write_text(MEMINFO_32G, encoding="ascii")
        self.golden = self.dir / "golden"
        self.golden.mkdir()
        self.vtr = self.dir / "vtr"
        self.vtr.mkdir()
        self.micros = self.dir / "tests" / "micro" / "verilog"
        self.work = self.dir / "work"
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.dict(os.environ, {"FAKE_LOG": str(self.log),
                                                         "FAKE_PID": str(self.pid_file)}))
        stack.enter_context(mock.patch.object(memcap, "MEMINFO", str(self.meminfo)))
        stack.enter_context(mock.patch.object(memcap, "SYSTEMD_RUN", "no-such-systemd-run"))
        self.vtr_state = stack.enter_context(
            mock.patch.object(exit_check, "vtr_state", return_value=(COMMIT, 0)))
        self.addCleanup(stack.close)

    def prov(self, blif: Path, design: Path, status: str = "ok",
             **override: str) -> dict[str, str]:
        """Write the golden's ``.prov`` as run-oracle.sh does (``override`` replaces a field; an
        empty value drops it)."""
        fields = {"vtr_commit": COMMIT, "vtr_dirty_files": "0",
                  "arch": "vtr_flow/arch/a.xml", "arch_sha256": sha(self.arch),
                  "source": "x/" + design.name, "source_sha256": sha(design), "tool": "parmys",
                  "status": status}
        if status == "ok" and blif.is_file():
            fields["blif_sha256"] = sha(blif)
        fields.update(override)
        text = "".join(f"{k}={v}\n" for k, v in fields.items() if v)
        blif.with_suffix(".prov").write_text(text, encoding="utf-8")
        return fields

    def micro(self, group: str, leaf: str, text: str, top: str | None = "top",
              golden_text: str | None = None, **prov: str) -> Path:
        """A micro source and (unless ``top`` is None) its Parmys golden with a matching
        ``.prov``."""
        path = self.micros / group / f"{leaf}.v"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        if top is not None:
            blif = self.golden / "regression" / "verilog" / group / leaf / f"{leaf}.parmys.blif"
            blif.parent.mkdir(parents=True, exist_ok=True)
            blif.write_text(golden_text or f".model {top}\n.end\n", encoding="utf-8")
            self.prov(blif, path, **prov)
        return path

    def rejected(self, group: str, leaf: str, log_text: str, text: str = "x\n") -> Path:
        """A micro the oracle failed on: a ``failed`` ``.prov`` and the oracle's log."""
        path = self.micro(group, leaf, text, top=None)
        blif = self.golden / "regression" / "verilog" / group / leaf / f"{leaf}.parmys.blif"
        blif.parent.mkdir(parents=True, exist_ok=True)
        self.prov(blif, path, status="failed")
        blif.with_suffix(".log").write_text(log_text, encoding="utf-8")
        return path

    def run_tool(self, *args: str, inputs: Sequence[Path] | None = None) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--arch", str(self.arch),
                "--sp-splice", str(self.splice), "--netlist-compare", str(self.compare),
                "--equiv-check", str(self.equiv), "--golden-root", str(self.golden),
                "--vtr-root", str(self.vtr), "--work", str(self.work), "--root", str(self.micros),
                *([] if "--vtr-config-template" in args else ["--no-config"]), *args,
                *map(str, inputs if inputs is not None else [self.micros])]
        return helpers.run_main(exit_check.main, argv)

    def calls(self, tool: str | None = None) -> list[list[str]]:
        if not self.log.is_file():
            return []
        rows = [json.loads(line) for line in self.log.read_text().splitlines()]
        return [row for row in rows if tool is None or row[0] == tool]

    def expect(self, text: str, name: str = "expect.txt") -> str:
        path = self.dir / name
        path.write_text(text, encoding="utf-8")
        return str(path)


XCONES = ("o[3]", "o[4]")


def eqx_record(rel: str, cones: Sequence[str] = XCONES, count: int | None = None,
               reason: str = "reviewed: reads an x memory bit") -> str:
    """An eq-x record text for the design ``rel`` with outputs ``cones``."""
    digest = exit_check.cones_digest(cones)
    lines = [f"design {rel} {len(cones) if count is None else count} {digest} {reason}"]
    lines += [f"bus {rel} {bus} {n}" for bus, n in exit_check.bus_counts(cones).items()]
    return "\n".join(lines) + "\n"


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
        self.assertIn("5 designs: PROVEN 2 (1 identical, 1 equivalent, 0 eq-x with a reviewed "
                      "record), 0 listed-reject; NOT PROVEN 0 unproven (listed); NOT CHECKED 0 "
                      "no-golden; 3 failing", out)

    def test_identical_and_equivalent_exit_0(self) -> None:
        self.micro("g", "a", "IDENT\n")
        self.micro("g", "b", "EQV\n")
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        self.assertIn("2 designs: PROVEN 2 (1 identical, 1 equivalent", out)

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
        self.assertIn("equiv-check: x.blif: unsupported: latch type 'ah'", out)


class EqxTest(ExitCheckCase):
    def test_an_unlisted_eq_x_does_not_count(self) -> None:
        self.micro("g", "x", "XCONE\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("(no reviewed record: does not count)", out)

    def test_a_recorded_eq_x_counts_when_the_outputs_are_exactly_the_record(self) -> None:
        self.micro("g", "x", "XCONE\n")
        record = self.expect(eqx_record("g/x.v"), "eqx.txt")
        code, out, err = self.run_tool("--eqx-record", record)
        self.assertEqual(code, 0, out + err)
        self.assertIn("eq-x (rec) g/x.v", out)
        self.assertIn("2 outputs as reviewed: reviewed: reads an x memory bit", out)
        self.assertIn("PROVEN 1 (0 identical, 0 equivalent, 1 eq-x with a reviewed record)", out)
        cones = (self.work / "0000-g_x.v" / "x-cones.txt").read_text()
        self.assertEqual(cones.split(), ["o[3]", "o[4]"])  # every excluded output is kept

    def test_a_record_that_differs_from_the_observed_outputs_fails(self) -> None:
        self.micro("g", "x", "XCONE\n")
        for name, record in (("fewer", eqx_record("g/x.v", ("o[3]",))),
                             ("other", eqx_record("g/x.v", ("o[3]", "p[0]"))),
                             ("more", eqx_record("g/x.v", ("o[3]", "o[4]", "o[5]")))):
            with self.subTest(name):
                code, out, _ = self.run_tool("--eqx-record", self.expect(record, "eqx.txt"))
                self.assertEqual(code, 1, out)
                self.assertIn("are not the reviewed record's", out)
                self.assertIn("FAIL eq-x, record differs", out)

    def test_a_record_for_a_design_that_is_equivalent_now_is_xpass(self) -> None:
        self.micro("g", "x", "EQV\n")
        code, out, _ = self.run_tool("--eqx-record", self.expect(eqx_record("g/x.v"), "e.txt"))
        self.assertEqual(code, 1)
        self.assertIn("XPASS      g/x.v", out)

    def test_a_malformed_record_is_a_usage_error(self) -> None:
        self.micro("g", "x", "XCONE\n")
        for text, message in ((eqx_record("g/x.v", count=5), "add up to 2, not 5"),
                              ("design g/x.v 2 abc\n", "expected 'design <path>"),
                              ("bus g/x.v o 2\n", "without a design line"),
                              (eqx_record("g/x.v") * 2, "recorded twice")):
            with self.subTest(message):
                code, _, err = self.run_tool("--eqx-record", self.expect(text, "e.txt"))
                self.assertEqual(code, 2)
                self.assertIn(message, err)


class UnprovenTest(ExitCheckCase):
    def test_unproven_is_its_own_class_and_fails_unless_listed(self) -> None:
        self.micro("g", "ok", "IDENT\n")
        self.micro("g", "slow", "EQUNPROVEN\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("unproven   g/slow.v", out)
        self.assertIn("ABC dsec timed out after 600 seconds", out)
        self.assertIn("FAIL unproven, not listed", out)
        self.assertNotIn("differs", out)

    def test_a_listed_unproven_design_exits_0_in_its_own_bucket_and_is_not_proven(self) -> None:
        self.micro("g", "ok", "IDENT\n")
        self.micro("g", "slow", "EQUNPROVEN\n")
        listing = self.expect("g/slow.v [timed out after 600 seconds] ABC does not finish\n")
        code, out, err = self.run_tool("--expect-unproven", listing)
        self.assertEqual(code, 0, out + err)
        self.assertIn("unproven   g/slow.v", out)
        self.assertIn("UNPROVEN (listed; not proven)", out)
        self.assertIn("PROVEN 1 (1 identical", out)
        self.assertIn("NOT PROVEN 1 unproven (listed)", out)

    def test_an_inconclusive_guess_is_unproven_too(self) -> None:
        self.micro("g", "guess", "EQGUESS\n")
        code, out, _ = self.run_tool("--expect-unproven",
                                     self.expect("g/guess.v [inconclusive] guessed pairs\n"),
                                     "--min-checked", "0")
        self.assertEqual(code, 0, out)

    def test_a_listed_unproven_design_that_now_passes_is_xpass(self) -> None:
        self.micro("g", "slow", "EQV\n")
        code, out, _ = self.run_tool("--expect-unproven", self.expect("g/slow.v x\n"))
        self.assertEqual(code, 1)
        self.assertIn("XPASS      g/slow.v", out)

    def test_a_listed_unproven_design_that_differs_still_fails(self) -> None:
        self.micro("g", "d", "nothing\n")
        code, out, _ = self.run_tool("--expect-unproven", self.expect("g/d.v x\n"))
        self.assertEqual(code, 1)
        self.assertIn("(listed, but not for this result)", out)

    def test_the_unproven_needle_must_match(self) -> None:
        self.micro("g", "slow", "EQUNPROVEN\n")
        code, out, _ = self.run_tool("--expect-unproven",
                                     self.expect("g/slow.v [clock domains] two clocks\n"))
        self.assertEqual(code, 1)
        self.assertIn("fails differently from the listing", out)

    def test_a_path_in_two_lists_is_a_usage_error(self) -> None:
        self.micro("g", "slow", "EQUNPROVEN\n")
        code, _, err = self.run_tool("--expect-unproven", self.expect("g/slow.v a\n", "u.txt"),
                                     "--expect-fail", self.expect("g/slow.v b\n", "r.txt"))
        self.assertEqual(code, 2)
        self.assertIn("'g/slow.v' is in more than one exception list", err)


class HardAndErrorTest(ExitCheckCase):
    def test_exit_1_counts_as_differs_only_with_the_verdict_line(self) -> None:
        self.micro("g", "real", "plain difference\n")
        self.micro("g", "prlimit", "EQPRLIMIT\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("differs    g/real.v", out)
        self.assertIn("error      g/prlimit.v", out)
        self.assertIn("equiv-check exited 1 without a NOT-equivalent verdict", out)
        self.assertIn("prlimit: failed to set the AS resource limit", out)
        self.assertEqual(len([ln for ln in out.splitlines() if ln.startswith("differs    ")]), 1)

    def test_a_python_crash_in_equiv_check_is_a_hard_error_never_listable(self) -> None:
        self.micro("g", "crash", "EQCRASH\n")
        code, out, _ = self.run_tool("--expect-fail", self.expect("g/crash.v [Traceback] x\n"))
        self.assertEqual(code, 3, out)
        self.assertIn("hard       g/crash.v", out)
        self.assertIn("never an expected result", out)

    def test_an_abc_signal_reported_by_equiv_check_is_hard_exit_3(self) -> None:
        self.micro("g", "abc", "EQHARD\n")
        code, out, _ = self.run_tool("--expect-unproven", self.expect("g/abc.v [ABC] x\n"))
        self.assertEqual(code, 3, out)
        self.assertIn("hard       g/abc.v", out)
        self.assertIn("ABC killed by signal 11", out)

    def test_equiv_check_dying_of_a_signal_is_hard(self) -> None:
        self.micro("g", "segv", "EQSEGV\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 3, out)
        self.assertIn("hard       g/segv.v", out)
        self.assertIn("killed by signal 11", out)

    def test_a_hard_failure_beats_a_plain_failure_in_the_exit_code(self) -> None:
        self.micro("g", "plain", "nothing\n")
        self.micro("g", "segv", "EQSEGV" + " " * 20 + "\n")
        self.assertEqual(self.run_tool()[0], 3)


class ZeroCheckedTest(ExitCheckCase):
    def test_a_missing_golden_root_is_a_usage_error(self) -> None:
        self.micro("g", "a", "IDENT\n")
        code, _, err = self.run_tool("--golden-root", str(self.dir / "nowhere"))
        self.assertEqual(code, 2)
        self.assertIn("the golden root is not a directory", err)
        self.assertEqual(self.calls(), [])

    def test_an_empty_golden_tree_checks_nothing_and_fails(self) -> None:
        self.micro("g", "a", "IDENT\n", top=None)
        self.micro("g", "b", "IDENT\n", top=None)
        code, out, err = self.run_tool()
        self.assertEqual(code, 1, out + err)
        self.assertIn("2 designs: PROVEN 0", out)
        self.assertIn("NOT CHECKED 2 no-golden", out)
        self.assertIn("only 0 design(s) proven, --min-checked is 1", out)
        self.assertEqual(self.calls(), [])

    def test_only_listed_exceptions_checks_nothing_and_fails(self) -> None:
        self.rejected("g", "r", "ERROR: boom\n")
        listing = self.expect("g/r.v [Parmys rejected the design] boom\n")
        code, out, _ = self.run_tool("--expect-fail", listing)
        self.assertEqual(code, 1)
        self.assertIn("only 0 design(s) proven", out)
        self.assertEqual(self.run_tool("--expect-fail", listing, "--min-checked", "0")[0], 0)

    def test_min_checked_is_a_floor(self) -> None:
        self.micro("g", "a", "IDENT\n")
        self.micro("g", "b", "EQV\n")
        self.assertEqual(self.run_tool("--min-checked", "2")[0], 0)
        code, out, _ = self.run_tool("--min-checked", "3")
        self.assertEqual(code, 1)
        self.assertIn("only 2 design(s) proven, --min-checked is 3", out)
        self.assertEqual(self.run_tool("--min-checked", "-1")[0], 2)


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
        compare = next(c for c in self.calls("netlist-compare") if "/a/" in c[2])
        self.assertEqual(compare[1], "--implicit-models")
        self.assertTrue(compare[2].endswith("g/a/a.parmys.blif"))
        self.assertTrue(compare[3].endswith("out.blif"))
        equiv = self.calls("equiv-check")[0]  # only the design netlist-compare did not settle
        self.assertEqual(equiv[1:3], ["--x-report", "--match-unnamed"])
        self.assertTrue(equiv[-2].endswith("g/b/b.parmys.blif"))
        self.assertEqual(len(self.calls("equiv-check")), 1)

    def test_every_child_including_sp_splice_runs_through_memcap(self) -> None:
        self.micro("g", "a", "EQV\n")
        with mock.patch.object(memcap, "run", wraps=memcap.run) as run:
            self.assertEqual(self.run_tool("--mem-cap", "2G")[0], 0)
        capped = {Path(call.args[0][0]).name: call.args[1] for call in run.call_args_list}
        self.assertEqual(capped["sp-splice"], 2 * GIB)
        self.assertEqual(capped["netlist-compare"], 2 * GIB)
        self.assertEqual(capped["equiv-check"], 2 * GIB)
        for call in run.call_args_list:  # the timeout, which kills the process group
            self.assertEqual(call.kwargs["timeout"], exit_check.TIMEOUT_S)

    def test_an_sp_splice_timeout_leaves_no_orphaned_child(self) -> None:
        """A hanging sp-splice and its sleeping child: the timeout kills the whole group."""
        self.micro("g", "hang", "SPLICEHANG\n")
        with mock.patch.object(exit_check, "TIMEOUT_S", 2):
            code, out, err = self.run_tool()
        self.assertEqual(code, 3, out + err)
        self.assertIn("hard       g/hang.v", out)
        self.assertIn("timed out after 2 s", out)
        pid = int(self.pid_file.read_text())
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and _alive(pid):
            time.sleep(0.1)
        self.assertFalse(_alive(pid), "the sp-splice child outlived the timeout")

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
                "--golden-root", str(self.golden), str(self.micros)])
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
            self.prov(blif, bench / (rel.split("/")[-1] + ".v") if "koios" not in rel
                      else bench / "koios" / "gemm.v")
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
        self.assertIn("2 designs: PROVEN 2 (2 identical", out)
        self.assertNotIn("nogold", out)
        self.assertNotIn("g/c.v", out)


def _alive(pid: int) -> bool:
    """True while ``pid`` is a running (not zombie) process."""
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except (OSError, IndexError):
        return False
    return state != "Z"


class ProvenanceTest(ExitCheckCase):
    def mismatch(self, **prov: str) -> str:
        self.micro("g", "a", "IDENT\n", **prov)
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1, out)
        self.assertIn("error      g/a.v", out)
        self.assertIn("golden provenance mismatch", out)
        self.assertEqual(self.calls(), [], "no comparison against a golden of other origin")
        return out

    def test_a_matching_golden_passes(self) -> None:
        self.micro("g", "a", "IDENT\n")
        self.assertEqual(self.run_tool()[0], 0)

    def test_each_provenance_field_is_checked(self) -> None:
        self.assertIn("vtr_commit golden=aaaaaaaaaaaa current=3c9a4d23b27d",
                      self.mismatch(vtr_commit="a" * 40))
        self.assertIn("arch_sha256 golden=000000000000", self.mismatch(arch_sha256="0" * 64))
        self.assertIn("source_sha256 golden=000000000000", self.mismatch(source_sha256="0" * 64))
        self.assertIn("blif_sha256 golden=000000000000", self.mismatch(blif_sha256="0" * 64))
        self.assertIn("vtr_dirty_files=3", self.mismatch(vtr_dirty_files="3"))
        self.assertIn("vtr_commit golden=missing", self.mismatch(vtr_commit=""))

    def test_the_message_says_how_to_fix_it(self) -> None:
        self.assertIn("regenerate it with tools/run-oracle.sh", self.mismatch(arch_sha256="0" * 64))

    def test_a_golden_without_a_prov_is_an_error(self) -> None:
        path = self.micro("g", "a", "IDENT\n")
        blif = self.golden / "regression" / "verilog" / "g" / "a" / "a.parmys.blif"
        blif.with_suffix(".prov").rename(self.dir / "moved.prov")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("has no provenance record", out)
        self.assertTrue(path.is_file())

    def test_a_rejected_designs_provenance_is_checked_too(self) -> None:
        path = self.rejected("g", "r", "ERROR: boom\n")
        blif = self.golden / "regression" / "verilog" / "g" / "r" / "r.parmys.blif"
        self.prov(blif, path, status="failed", vtr_commit="b" * 40)
        listing = self.expect("g/r.v [boom] boom\n")
        code, out, _ = self.run_tool("--expect-fail", listing)
        self.assertEqual(code, 1)
        self.assertIn("golden provenance mismatch", out)

    def test_a_comment_only_source_difference_is_accepted_with_the_upstream_file(self) -> None:
        """A micro with an added ``// elab-check:`` line: the upstream file still has the hash."""
        upstream = self.vtr / "x" / "a.v"
        upstream.parent.mkdir()
        upstream.write_text("module top; // IDENT\nendmodule", encoding="utf-8")
        text = "module top; // IDENT\nendmodule\n// elab-check: --yosys-tribuf\n"
        path = self.micro("g", "a", text, source_sha256=sha(upstream), source="x/a.v")
        self.assertNotEqual(sha(path), sha(upstream))
        self.assertEqual(self.run_tool()[0], 0)
        upstream.write_text("module top2;\nendmodule", encoding="utf-8")  # no longer the hash
        blif = self.golden / "regression" / "verilog" / "g" / "a" / "a.parmys.blif"
        self.prov(blif, path, source="x/a.v", source_sha256="1" * 64)
        self.assertEqual(self.run_tool()[0], 1)

    def test_a_real_difference_is_not_accepted_by_the_comment_rule(self) -> None:
        upstream = self.vtr / "x" / "a.v"
        upstream.parent.mkdir()
        upstream.write_text("module top;\nendmodule", encoding="utf-8")
        self.micro("g", "a", "module other;\nendmodule\n", source_sha256=sha(upstream),
                   source="x/a.v")
        self.assertEqual(self.run_tool()[0], 1)

    def test_no_vtr_checkout_is_a_usage_error(self) -> None:
        self.micro("g", "a", "IDENT\n")
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--arch", str(self.arch),
                "--no-config", "--golden-root", str(self.golden), str(self.micros)]
        with mock.patch.dict(os.environ):
            os.environ.pop("VTR_ROOT", None)
            code, _, err = helpers.run_main(exit_check.main, argv)
        self.assertEqual(code, 2)
        self.assertIn("the VTR checkout is unknown", err)

    def test_a_dirty_vtr_tree_is_a_usage_error(self) -> None:
        self.micro("g", "a", "IDENT\n")
        self.vtr_state.return_value = (COMMIT, 2)
        code, _, err = self.run_tool()
        self.assertEqual(code, 2)
        self.assertIn("2 modified file(s)", err)

    def test_vtr_state_reads_a_real_git_checkout(self) -> None:
        env = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
               "GIT_COMMITTER_EMAIL": "t@t", "GIT_CONFIG_GLOBAL": os.devnull,
               "GIT_CONFIG_SYSTEM": os.devnull, "PATH": os.environ["PATH"]}
        repo = self.dir / "repo"
        repo.mkdir()
        (repo / "f.txt").write_text("x\n", encoding="utf-8")
        for words in (["init", "-q"], ["add", "f.txt"], ["commit", "-q", "-m", "m"]):
            subprocess.run(["git", "-C", str(repo), *words], check=True, env=env,
                           capture_output=True)
        commit, dirty = _real_vtr_state(repo)
        self.assertRegex(commit, "^[0-9a-f]{40}$")
        self.assertEqual(dirty, 0)
        (repo / "f.txt").write_text("y\n", encoding="utf-8")
        self.assertEqual(_real_vtr_state(repo)[1], 1)
        with self.assertRaises(exit_check.UsageError):
            _real_vtr_state(self.dir)


_REAL_VTR_STATE = exit_check.vtr_state


def _real_vtr_state(root: Path) -> tuple[str, int]:
    return _REAL_VTR_STATE(root)


class ListedTest(ExitCheckCase):
    def test_a_design_the_oracle_never_ran_on_is_no_golden_not_an_error(self) -> None:
        self.micro("g", "ok", "IDENT\n")
        self.micro("g", "koios", "x\n", top=None)
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        self.assertIn("no-golden  g/koios.v", out)
        self.assertIn("koios.parmys.blif does not exist", out)
        self.assertIn("NOT CHECKED 1 no-golden", out)
        self.assertIn("2 designs: PROVEN 1", out)
        self.assertEqual(len(self.calls("sp-splice")), 1)
        self.assertNotIn("error      g/koios.v", out)

    def test_a_rejected_design_fails_unless_listed_with_its_needle(self) -> None:
        self.rejected("g", "rej", "ERROR: bad\n")
        self.micro("g", "ok", "IDENT\n")
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("reject     g/rej.v", out)
        self.assertIn("Parmys rejected the design (status=failed)", out)
        self.assertIn("FAIL reject, not listed", out)
        self.assertEqual(self.calls("sp-splice")[0][-1].endswith("ok.v"), True)
        listing = self.expect("g/rej.v [Parmys rejected the design] Parmys rejects it: x\n")
        code, out, _ = self.run_tool("--expect-fail", listing)
        self.assertEqual(code, 0, out)
        self.assertIn("listed     g/rej.v", out)
        self.assertIn("1 listed-reject", out)
        self.assertEqual(len(self.calls("sp-splice")), 2)  # only the good one, per run

    def test_the_oracles_log_is_where_a_reject_needle_can_match(self) -> None:
        self.rejected("g", "rej", "ERROR: Duplicate definition of module dual_port_ram!\n")
        self.micro("g", "ok", "IDENT\n")
        listing = self.expect("g/rej.v [Duplicate definition of module] Parmys rejects it\n")
        self.assertEqual(self.run_tool("--expect-fail", listing)[0], 0)
        wrong = self.expect("g/rej.v [Can't open include file] Parmys rejects it\n")
        code, out, _ = self.run_tool("--expect-fail", wrong)
        self.assertEqual(code, 1)
        self.assertIn("fails differently from the listing", out)

    def test_an_oracle_harness_gap_is_no_golden_never_a_pass(self) -> None:
        self.rejected("g", "inc", "ERROR: Can't open include file `assign.vh'!\n")
        self.micro("g", "ok", "IDENT\n")
        listing = self.expect("g/inc.v [Can't open include file] oracle harness gap: no -include\n")
        code, out, err = self.run_tool("--expect-no-golden", listing)
        self.assertEqual(code, 0, out + err)
        self.assertIn("no-golden  g/inc.v", out)
        self.assertIn("oracle harness gap: no -include", out)
        self.assertIn("no-golden (harness gap, listed)", out)
        self.assertIn("NOT CHECKED 1 no-golden", out)
        self.assertIn("0 listed-reject", out)
        self.assertIn("PROVEN 1 (1 identical", out)
        code, out, _ = self.run_tool("--expect-no-golden",
                                     self.expect("g/inc.v [undefined macro] -D\n"))
        self.assertEqual(code, 1)
        self.assertIn("fails differently from the listing", out)

    def test_a_capped_golden_is_an_error_not_listable(self) -> None:
        path = self.micro("g", "big", "x\n", top=None)
        blif = self.golden / "regression" / "verilog" / "g" / "big" / "big.parmys.blif"
        blif.parent.mkdir(parents=True)
        self.prov(blif, path, status="capped")
        listing = self.expect("g/big.v [capped] x\n")
        code, out, _ = self.run_tool("--expect-fail", listing)
        self.assertEqual(code, 1)
        self.assertIn("Parmys run capped (no verdict)", out)
        self.assertIn("(listed, but not for this result)", out)

    def test_a_listed_design_that_passes_is_xpass(self) -> None:
        self.micro("g", "a", "IDENT\n")
        code, out, _ = self.run_tool("--expect-fail", self.expect("g/a.v because\n"))
        self.assertEqual(code, 1)
        self.assertIn("XPASS      g/a.v", out)
        self.assertIn("FAIL listed but passing", out)

    def test_differs_and_error_are_never_listable(self) -> None:
        self.micro("g", "d", "nothing\n")
        self.micro("g", "e", "SPLICEFAIL" + " " * 10 + "\n")
        listing = self.expect("g/d.v [NOT equivalent outside] proven: x\n"
                              "g/e.v [two drivers] reason\n")
        code, out, _ = self.run_tool("--expect-fail", listing)
        self.assertEqual(code, 1)
        self.assertEqual(out.count("(listed, but not for this result)"), 2)

    def test_a_signal_or_cap_kill_is_never_expected(self) -> None:
        self.micro("g", "oom", "SPLICEKILL\n")
        self.micro("g", "segv", "SPLICESEGV\n")
        self.micro("g", "nc", "NCKILL\n")
        listing = self.expect("g/oom.v [Yosys] x\ng/segv.v [odin3] x\ng/nc.v [exit] x\n")
        code, out, _ = self.run_tool("--expect-fail", listing, "--mem-cap", "3G")
        self.assertEqual(code, 3)
        self.assertIn("out of memory (cap 3G)", out)
        self.assertEqual(out.count("is never an expected result"), 3)
        self.assertIn("FAIL hard (crash/kill/timeout)", out)

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
        m.assert_called()

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
        for flag in ("--expect-fail", "--expect-unproven", "--expect-no-golden", "--eqx-record",
                     "--min-checked", "--vtr-root"):
            self.assertIn(flag, out)
        code, _, err = helpers.run_main(exit_check.main, [
            "--odin3", str(self.odin3), "--yosys", "no-such-yosys", "--arch", str(self.arch),
            "--golden-root", str(self.golden), "--no-config", str(self.micros)])
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

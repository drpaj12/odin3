"""Tests for 3H plan Tasks 14-15: rerun-check, the Oprov probe, seeds and exit-run.

Nothing here runs VTR or a real odin3: the front end is a fake script run through the fake heavy
run of ``test_vtr_qor_sched.RunnerTestBase``.
"""

from __future__ import annotations

import dataclasses
import hashlib
import io
import json
import sys
import unittest
from collections.abc import Sequence
from contextlib import redirect_stderr
from datetime import date
from pathlib import Path
from unittest import mock

import qor_heavy
import qor_legs
import qor_nightly
import qor_sched
import vtr_qor
from qor_sched import Job

from .test_vtr_qor_cli import CliTestBase
from .test_vtr_qor_sched import KEY12, RunnerTestBase

FAKE_ODIN3 = """
import json, sys
from pathlib import Path
here = Path(__file__).resolve().parent
mode = json.loads((here / "mode.json").read_text())
script = sys.argv[sys.argv.index("-p") + 1]
out = Path(script.split()[-1])
with (here / "calls.jsonl").open("a") as log:
    log.write(json.dumps(str(out)) + "\\n")
out.write_bytes(mode["bytes"].encode())
sys.exit(mode.get("exit", 0))
"""

FAKE_TIME = """
import subprocess, sys
args = sys.argv[1:]
assert args[0] == "-v" and args[1] == "-o"
open(args[2], "w").write("fake time\\n")
sys.exit(subprocess.call(args[3:]))
"""


class RerunTest(RunnerTestBase):
    """Spec §8.3 / plan Task 14: ``run_rerun`` against a fake front end."""

    def setUp(self) -> None:
        super().setUp()
        self.bin = self.tmp / "bin"
        self.bin.mkdir()
        self.odin3 = self.bin / "odin3"
        self.odin3.write_text(f"#!{sys.executable}\n{FAKE_ODIN3}")
        self.odin3.chmod(0o755)
        fake_time = self.bin / "time"
        fake_time.write_text(f"#!{sys.executable}\n{FAKE_TIME}")
        fake_time.chmod(0o755)
        patch = mock.patch.object(qor_legs, "TIME_BIN", str(fake_time))
        patch.start()
        self.addCleanup(patch.stop)
        self.mode("a")

    def mode(self, text: str, **extra: object) -> None:
        (self.bin / "mode.json").write_text(json.dumps({"bytes": text, **extra}))

    def o_job(self, design: str = "sha", *, kept: bool = False, sidecar: bool = True) -> Job:
        job_dir = self.job_dir(design, "O")
        job_dir.mkdir(parents=True)
        ctx = qor_legs.LegContext(
            vtr_root=self.tmp / "vtr", python=Path(sys.executable), arch_xml=self.tmp / "a.xml",
            odin3=self.odin3, repo=qor_legs.REPO_ROOT, job_dir=job_dir, k=6, group="A")
        commands = tuple(qor_legs.build_commands("O", design, ctx))
        job = Job(leg="O", design=design, arch="k6", group="A", gating=True, key="f" * 64,
                  key12=KEY12, job_dir=job_dir, commands=commands, cached=False, ctx=ctx)
        first = b"a"
        digest = hashlib.sha256(first).hexdigest()
        if kept:
            (job_dir / f"{design}.blif").write_bytes(first)
        elif sidecar:
            (job_dir / f"{design}.blif.sha256").write_text(digest + "\n")
        (job_dir / "frontend.log").write_text("first run\n")
        (job_dir / "frontend.time").write_text("first time\n")
        (job_dir / "metrics.json").write_text(json.dumps({
            "schema": 1, "key": job.key, "status": "ok", "verdicts": {"rerun": "skipped"}}))
        return job

    def verdict(self, job: Job) -> str:
        return str(json.loads((job.job_dir / "metrics.json").read_text())["verdicts"]["rerun"])

    def test_rerun_check(self) -> None:
        """Identical bytes: same; one byte different: differs; rerun.blif is gone either way and
        the first run's frontend files are untouched."""
        for text, expected in (("a", "same"), ("b", "differs")):
            self.mode(text)
            job = self.o_job(f"sha{expected}")
            self.assertEqual(qor_sched.run_rerun(job, self.opts()), expected)
            self.assertEqual(self.verdict(job), expected)
            self.assertFalse((job.job_dir / "rerun.blif").exists())
            self.assertEqual((job.job_dir / "frontend.log").read_text(), "first run\n")
            self.assertEqual((job.job_dir / "frontend.time").read_text(), "first time\n")
            self.assertTrue((job.job_dir / "rerun.time").is_file())
        calls = [json.loads(x) for x in (self.bin / "calls.jsonl").read_text().splitlines()]
        self.assertEqual([Path(c).name for c in calls], ["rerun.blif", "rerun.blif"])

    def test_rerun_against_kept_netlist(self) -> None:
        job = self.o_job(kept=True)
        self.assertEqual(qor_sched.run_rerun(job, self.opts()), "same")
        self.assertEqual((job.job_dir / "sha.blif").read_bytes(), b"a")

    def test_rerun_without_reference_or_with_failing_front_end(self) -> None:
        err = io.StringIO()
        job = self.o_job(sidecar=False)
        with redirect_stderr(err):
            self.assertEqual(qor_sched.run_rerun(job, self.opts()), "error")
        self.assertIn("no first netlist", err.getvalue())
        self.assertEqual(self.verdict(job), "error")
        self.mode("a", exit=1)
        job2 = self.o_job("diffeq1")
        with redirect_stderr(io.StringIO()):
            self.assertEqual(qor_sched.run_rerun(job2, self.opts()), "error")
        self.assertFalse((job2.job_dir / "rerun.blif").exists())

    def test_rerun_refused_outside_heavy_run(self) -> None:
        with self.assertRaises(qor_heavy.HeavyError):
            qor_sched.run_rerun(self.o_job(), self.opts(scope=None))

    def test_rerun_command_swaps_only_the_outputs(self) -> None:
        job = self.o_job()
        cmd = qor_legs.rerun_command(job.commands[0], job.job_dir, "sha")
        old = job.commands[0]
        self.assertEqual(cmd.stdout, job.job_dir / "rerun.log")
        self.assertNotIn(str(job.job_dir / "sha.blif"), " ".join(cmd.argv))
        self.assertEqual(len(cmd.argv), len(old.argv))
        self.assertEqual(cmd.cwd, old.cwd)
        vtr = dataclasses.replace(old, argv=("/x/python", "run_vtr_flow.py"))
        with self.assertRaises(ValueError):
            qor_legs.rerun_command(vtr, job.job_dir, "sha")


class ExitCliTest(CliTestBase):
    """Plan Task 14 through the command line: rerun-check, --seed, the Oprov capability."""

    def test_rerun_check_cli(self) -> None:
        code, _, err = self.cli("run", "--leg", "O", "--designs", "ch_intrinsics,sha")
        self.assertEqual(code, 0, err)
        code, _, err = self.cli("rerun-check", "--designs", "ch_intrinsics,sha")
        self.assertEqual(code, 0, err)
        for design in ("ch_intrinsics", "sha"):
            (job,) = self.job_dirs("O", design)
            self.assertEqual(self.load(job / "metrics.json")["verdicts"]["rerun"], "same")
            self.assertFalse((job / "rerun.blif").exists())
        self.assertIn("vtr-qor: rerun-check done: 2 same", self.harness_lines()[-1])
        # the front end now writes other bytes: differs, exit 1
        (self.bin / "abc_line.txt").write_text("# abc: other\n")
        code, _, err = self.cli("rerun-check", "--designs", "sha")
        self.assertEqual(code, 1, err)
        (job,) = self.job_dirs("O", "sha")
        self.assertEqual(self.load(job / "metrics.json")["verdicts"]["rerun"], "differs")

    def test_rerun_check_missing_job(self) -> None:
        code, _, err = self.cli("rerun-check", "--designs", "sha")
        self.assertEqual(code, 1)
        self.assertIn("vtr-qor: rerun-check: O sha: no ok job; run it first\n", err)

    def test_seed_key(self) -> None:
        """--seed 2 is another key (and passes --seed 2 to the flow); seed 1 is the default."""
        def key(*extra: str) -> str:
            code, out, err = self.cli("run", "--leg", "P", "--designs", "sha", "--dry-run", *extra)
            self.assertEqual(code, 0, err)
            return out.split()[2]
        self.assertEqual(key("--seed", "1"), key())
        self.assertNotEqual(key("--seed", "2"), key())
        self.assertNotEqual(key("--seed", "2"), key("--seed", "3"))
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "sha", "--seed", "0",
                                  "--dry-run")[0], 2)

    def test_seed_run_never_moves_latest(self) -> None:
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "sha")[0], 0)
        leg_dir = self.out / self.pin[:12] / "k6_frac_N10_frac_chain_mem32K_40nm" / "P"
        first = (leg_dir / "latest").read_text()
        code, _, err = self.cli("run", "--leg", "P", "--designs", "sha", "--seed", "2")
        self.assertEqual(code, 0, err)
        self.assertEqual((leg_dir / "latest").read_text(), first)
        self.assertEqual(len([d for d in leg_dir.iterdir() if d.is_dir()]), 2)

    def test_oprov_needs_the_provenance_name_style(self) -> None:
        """The Oprov probe runs the real leg script: a refusing odin3 makes the leg unavailable
        (E-QH5), a working one makes it run; doctor says which."""
        with mock.patch.object(vtr_qor, "name_style_probe", return_value="not available yet"):
            code, _, err = self.cli("run", "--leg", "Oprov", "--designs", "sha")
            self.assertEqual(code, 2)
            self.assertIn("leg Oprov needs pass 'write_blif --name-style provenance'", err)
            _, out, _ = self.cli("doctor")
            self.assertIn("Oprov missing 'write_blif --name-style provenance'", out)
            self.assertEqual(vtr_qor._available_legs(self.odin3, on_request=("Oprov",)),
                             ("P", "O", "Oprime", "Pprime", "R"))
        with mock.patch.object(vtr_qor, "name_style_probe", return_value=None):
            self.assertEqual(self.cli("run", "--leg", "Oprov", "--designs", "sha")[0], 0)
            self.assertIn("Oprov", vtr_qor._available_legs(self.odin3, on_request=("Oprov",)))
            # the nightly never names Oprov in
            self.assertNotIn("Oprov", vtr_qor._available_legs(self.odin3))

    def test_probe_runs_the_oprov_script(self) -> None:
        """The real probe against the fake odin3: exit 0 and a file: capable; exit 1: the reason."""
        work = self.tmp / "probe"
        self.assertIsNone(qor_legs.probe_name_style(self.odin3, self.repo, work))
        self.assertTrue((work / "probe.v").is_file())
        (self.bin / "fail").write_text("")
        self.assertIsNotNone(qor_legs.probe_name_style(self.odin3, self.repo, work))


class NightRerunStepTest(unittest.TestCase):
    def test_step3_runs_rerun_check_on_group_a(self) -> None:
        """Task 13 left rerun-check a logged skip until Task 14: now it is step 3's last
        command, over the group A designs, and only when O can run."""
        sets = qor_sched.load_sets()
        for legs, expect in ((("P", "O", "Oprime"), True), (("P", "Oprime"), False)):
            seen: list[list[str]] = []

            def inner(argv: Sequence[str], seen: list[list[str]] = seen) -> int:
                seen.append(list(argv))
                return 0

            ctx = qor_nightly.NightContext(
                out=Path("/o"), repo=Path("/r"), odin3=Path("/x"), vtr_root=Path("/v"),
                golden_k6=Path("/g"), sets=sets, legs=legs, common=("--out", "/o"),
                today=date(2026, 10, 19),
                run_inner=inner,
                emit=lambda _m: None, refused=lambda: 0)
            qor_nightly.Night(ctx).step3(checks=True)
            reruns = [a for a in seen if a[0] == "rerun-check"]
            self.assertEqual(len(reruns), int(expect))
            if expect:
                group_a = ",".join(d for d in sets.order if sets.designs[d].group == "A")
                self.assertEqual(reruns[0], ["rerun-check", "--out", "/o", "--designs", group_a])
                self.assertEqual(seen[-1], reruns[0])


if __name__ == "__main__":
    unittest.main()

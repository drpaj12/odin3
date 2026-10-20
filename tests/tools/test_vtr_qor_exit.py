"""Tests for 3H plan Tasks 14-15: rerun-check, the Oprov probe, seeds and exit-run.

Nothing here runs VTR or a real odin3: the front end is a fake script run through the fake heavy
run of ``test_vtr_qor_sched.RunnerTestBase``.
"""

from __future__ import annotations

import dataclasses
import hashlib
import io
import json
import subprocess
import sys
import tempfile
import time
import unittest
from collections.abc import Sequence
from contextlib import redirect_stderr
from datetime import date, datetime
from pathlib import Path
from unittest import mock

import qor_exit
import qor_heavy
import qor_legs
import qor_nightly
import qor_sched
import qor_table
import vtr_qor
from qor_sched import Job

from .test_vtr_qor_cli import CliTestBase, _script
from .test_vtr_qor_sched import KEY12, FakeClock, RunnerTestBase
from .test_vtr_qor_table import BANDS, GATE, drop, header, replace, run, table

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
            vtr_root=self.tmp / "vtr",
            python=Path(sys.executable),
            arch_xml=self.tmp / "a.xml",
            odin3=self.odin3,
            repo=qor_legs.REPO_ROOT,
            job_dir=job_dir,
            k=6,
            group="A",
        )
        commands = tuple(qor_legs.build_commands("O", design, ctx))
        job = Job(
            leg="O",
            design=design,
            arch="k6",
            group="A",
            gating=True,
            key="f" * 64,
            key12=KEY12,
            job_dir=job_dir,
            commands=commands,
            cached=False,
            ctx=ctx,
        )
        first = b"a"
        digest = hashlib.sha256(first).hexdigest()
        if kept:
            (job_dir / f"{design}.blif").write_bytes(first)
        elif sidecar:
            (job_dir / f"{design}.blif.sha256").write_text(digest + "\n")
        (job_dir / "frontend.log").write_text("first run\n")
        (job_dir / "frontend.time").write_text("first time\n")
        (job_dir / "metrics.json").write_text(
            json.dumps(
                {"schema": 1, "key": job.key, "status": "ok", "verdicts": {"rerun": "skipped"}}
            )
        )
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
        self.assertEqual(
            self.cli("run", "--leg", "P", "--designs", "sha", "--seed", "0", "--dry-run")[0], 2
        )

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
            self.assertEqual(
                vtr_qor._available_legs(self.odin3, on_request=("Oprov",)),
                ("P", "O", "Oprime", "Pprime", "R"),
            )
        with mock.patch.object(vtr_qor, "name_style_probe", return_value=None):
            self.assertEqual(self.cli("run", "--leg", "Oprov", "--designs", "sha")[0], 0)
            self.assertIn("Oprov", vtr_qor._available_legs(self.odin3, on_request=("Oprov",)))
            # the nightly never names Oprov in
            self.assertNotIn("Oprov", vtr_qor._available_legs(self.odin3))

    def test_table_seeds_option(self) -> None:
        """``table --seeds 2,3`` finds the seed jobs by the planner's keys and adds the section."""
        self.assertEqual(self.cli("run", "--leg", "P,O", "--set", "smoke")[0], 0)
        for seed in ("2", "3"):
            code, _, err = self.cli("run", "--leg", "P,O", "--designs", "sha", "--seed", seed)
            self.assertEqual(code, 0, err)
        self.stack.enter_context(mock.patch.object(vtr_qor, "_golden", return_value=None))
        code, out, err = self.cli(
            "table", "--legs", "P,O", "--label", "s", "--set", "smoke", "--seeds", "2,3"
        )
        self.assertEqual(code, 0, err)
        md = (Path(out.strip().splitlines()[-1]) / "qor.md").read_text(encoding="utf-8")
        self.assertIn("## Seeds\n", md)
        self.assertEqual(self.cli("table", "--legs", "P,O", "--label", "s", "--seeds", "1")[0], 2)
        self.assertEqual(self.cli("table", "--legs", "P", "--label", "s", "--seeds", "2")[0], 2)
        self.assertEqual(self.cli("table", "--legs", "P,O", "--label", "s", "--seeds", "x")[0], 2)

    def test_exit_run_command(self) -> None:
        """``exit-run`` builds its steps from this odin3's legs (no Oprov), with the options every
        inner command takes, and its admission names itself ``exit-run``."""
        inner = FakeInner()
        seen: list[str] = []

        def record(argv: Sequence[str]) -> int:
            seen.append(vtr_qor.ADMISSION.owner if vtr_qor.ADMISSION is not None else "none")
            return inner(argv)

        with (
            mock.patch.object(vtr_qor, "run_inner", record),
            mock.patch.object(vtr_qor, "_exit_candidates", return_value=lambda: []),
            mock.patch.object(vtr_qor, "name_style_probe", return_value=None),
        ):
            code, _, err = self.cli("exit-run", "--label", "exit-20261019")
        self.assertEqual(code, 0, err)
        self.assertEqual(set(seen), {"exit-run"})
        self.assertIsNone(vtr_qor.ADMISSION)
        first = inner.calls[0]
        self.assertEqual(first[:2], ["run", "--vtr-root"])
        self.assertEqual(first[first.index("--leg") + 1], "P,Oprime,Pprime,O")
        self.assertEqual(self.cli("exit-run", "--label", "bad label")[0], 2)
        self.assertEqual(
            self.cli("exit-run", "--label", "x", "--odin3", "/nonexistent/odin3", common=False)[0],
            2,
        )

    def test_crosscheck_without_a_harness_job(self) -> None:
        """A task design the harness has no P job for gets one clear line, not a line per key."""
        task = self.tmp / "task"
        task.mkdir()
        (self.vtr / "task-results").write_text(
            "circuit\tvpr_status\tnum_clb\t\nch_intrinsics.v\tsuccess\t12\t\n"
        )
        code, _, err = self.cli("crosscheck", "--task", str(task))
        self.assertEqual(code, 1)
        self.assertEqual(
            err,
            "vtr-qor: crosscheck: ch_intrinsics: no harness P job; run "
            "vtr-qor run --leg P --designs ch_intrinsics\n",
        )

    def test_exit_keys_never_latest(self) -> None:
        """Review H1: with an older complete key in place, the exit run's keys are those of the
        current odin3, and the candidates are not decided from the older key."""
        self.assertEqual(self.cli("run", "--leg", "P,O", "--set", "smoke")[0], 0)
        out = self.out
        old = vtr_qor.read_latest(out / self.pin[:12] / "k6_frac_N10_frac_chain_mem32K_40nm" / "O")
        self.assertIsNotNone(old)
        self.odin3.write_text(self.odin3.read_text() + "\n# rebuilt\n")
        args = vtr_qor._parser().parse_args(["exit-run", "--label", "x"])
        sets = qor_sched.load_sets()
        keys = vtr_qor._exit_keys(args, out, self.vtr, self.odin3, ("P", "O"), sets)()
        self.assertNotEqual(keys["O"], old)
        self.assertEqual(vtr_qor._exit_candidates(args, out, lambda: keys)(), None)

    def test_probe_runs_the_oprov_script(self) -> None:
        """The real probe against the fake odin3: exit 0 and a file: capable; exit 1: the reason."""
        work = self.tmp / "probe"
        self.assertIsNone(qor_legs.probe_name_style(self.odin3, self.repo, work))
        self.assertTrue((work / "probe.v").is_file())
        (self.bin / "fail").write_text("")
        self.assertIsNotNone(qor_legs.probe_name_style(self.odin3, self.repo, work))

    def test_probe_rejects_an_odin3_that_ignores_the_style(self) -> None:
        """Review M2: exit 0 and a file for both scripts, but the same names: not capable."""
        same = _script(
            self.bin / "same-odin3",
            (
                "import sys\nfrom pathlib import Path\n"
                "script = sys.argv[sys.argv.index('-p') + 1]\n"
                "Path(script.split()[-1]).write_text('.model m\\n.end\\n')\n"
            ),
        )
        reason = qor_legs.probe_name_style(same, self.repo, self.tmp / "probe2")
        self.assertEqual(reason, "--name-style provenance wrote the same names as the default")
        with mock.patch.object(vtr_qor, "name_style_probe", wraps=vtr_qor.name_style_probe):
            self.assertEqual(
                vtr_qor.leg_lacks("Oprov", same, frozenset(qor_legs.LEG_PASSES["O"])),
                vtr_qor.NAME_STYLE_PASS,
            )

    def test_name_style_probe_cleans_up(self) -> None:
        """Review M3: the probe's directory is gone afterwards (through safe_rmtree)."""
        self.assertIsNone(vtr_qor.name_style_probe(self.odin3))
        self.assertFalse(list((qor_heavy.HEAVY_DIR / "vtr-qor-probe").glob("*")))


class SeedTableTest(unittest.TestCase):
    """Plan Task 14: ``seed_candidates`` and the Seeds and Name style sections."""

    def test_seed_candidates(self) -> None:
        # 1.36 for sha: listed (>= 1.35); nothing else is
        runs = table(o_scales={"sha": 1.36})
        self.assertEqual(qor_table.seed_candidates(runs, BANDS), ["sha"])
        # exactly 1.35 is listed, 1.349 is not
        self.assertEqual(qor_table.seed_candidates(table(o_scales={"sha": 1.35}), BANDS), ["sha"])
        self.assertEqual(qor_table.seed_candidates(table(o_scales={"sha": 1.349}), BANDS), [])
        # a geomean of 1.09 (>= 1.08) with raygentop at 1.12: it is above, the rest below
        rest = (1.09**15 / 1.12) ** (1 / 14)
        scales = {d: rest for d in GATE}
        scales["raygentop"] = 1.12
        self.assertEqual(qor_table.seed_candidates(table(o_scales=scales), BANDS), ["raygentop"])
        # everything below: empty
        self.assertEqual(qor_table.seed_candidates(table(1.05), BANDS), [])
        # a failed O job never makes a candidate; an absent P job neither
        runs = replace(table(o_scales={"sha": 1.4}), run("O", "sha", 1.4, status="failed"))
        self.assertEqual(qor_table.seed_candidates(runs, BANDS), [])
        self.assertEqual(
            qor_table.seed_candidates(drop(table(o_scales={"sha": 1.4}), "P", "sha"), BANDS), []
        )

    def write(self, runs: list[qor_table.Run], seeds: dict[int, list[qor_table.Run]] | None) -> str:
        with tempfile.TemporaryDirectory(dir=qor_heavy.HEAVY_DIR) as tmp:
            path = Path(tmp) / "qor.md"
            verdict = qor_table.gate(
                qor_table.GateInputs(
                    runs=runs,
                    bands=BANDS,
                    gate_set=GATE,
                    explanations=[],
                    divergences=[],
                    equiv_exceptions=[],
                )
            )
            qor_table.write_md(runs, verdict, header(runs), path, golden=None, seeds=seeds)
            return path.read_text(encoding="utf-8")

    def test_md_seed_and_name_style(self) -> None:
        runs = table(o_scales={"sha": 1.36})
        for design in GATE:
            runs.append(
                run("Oprov", design, override={"vpr_time_s": 2.0} if design == "sha" else None)
            )
        seeds = {
            2: [run("O", "sha", 1.30), run("P", "sha")],
            3: [run("O", "sha", 1.34), run("P", "sha")],
        }
        md = self.write(runs, seeds)
        seed_block = (
            "## Seeds\n\n"
            "Designs within 10 % of a bound (O/P at least 1.35 per design, or above a geomean "
            "of at least 1.08) rerun with VPR seeds 2 and 3 for O and P. The gate stays on "
            "seed 1.\n\n"
            "| design | metric | O/P s1 (gate) | O/P s2 | O/P s3 | mean(s1..s3) |\n"
            "|---|---|---|---|---|---|\n"
            "| sha | num_clb | 1.360 | 1.300 | 1.340 | 1.333 |\n"
            "| sha | min_chan_width | 1.360 | 1.300 | 1.340 | 1.333 |\n"
            "| sha | critical_path_delay_ns | 1.360 | 1.300 | 1.340 | 1.333 |\n"
            "| sha | crit_path_routed_wirelength | 1.360 | 1.300 | 1.340 | 1.333 |\n\n"
        )
        self.assertIn(seed_block, md)
        self.assertLess(md.index("## Seeds"), md.index("## Netlist"))
        self.assertIn("| **geomean** |  |  | **1.047** |  |  |  |\n", md)
        self.assertIn("| sha | 1.0 | 2.0 | 2.000 |  |  |  |\n", md)
        # no seed runs: no Seeds section; a seed missing: its cell is empty and so is the mean
        self.assertNotIn("## Seeds", self.write(runs, None))
        partial = self.write(runs, {2: seeds[2]})
        self.assertIn("| sha | num_clb | 1.360 | 1.300 |  |  |\n", partial)
        none = self.write(table(1.05), {2: [], 3: []})
        self.assertIn("None.\n", none.split("## Seeds")[1].split("##")[0])


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
                out=Path("/o"),
                repo=Path("/r"),
                odin3=Path("/x"),
                vtr_root=Path("/v"),
                golden_k6=Path("/g"),
                sets=sets,
                legs=legs,
                common=("--out", "/o"),
                today=date(2026, 10, 19),
                run_inner=inner,
                emit=lambda _m: None,
                refused=lambda: 0,
            )
            qor_nightly.Night(ctx).step3(checks=True)
            reruns = [a for a in seen if a[0] == "rerun-check"]
            self.assertEqual(len(reruns), int(expect))
            if expect:
                group_a = ",".join(d for d in sets.order if sets.designs[d].group == "A")
                self.assertEqual(
                    reruns[0], ["rerun-check", "--out", "/o", "--designs", group_a, "--resume"]
                )
                self.assertEqual(seen[-1], reruns[0])


SETS = qor_sched.load_sets()
TASKS = qor_legs.REPO_ROOT / "tests" / "vtr" / "tasks"
COMMON = ("--vtr-root", "/v", "--odin3", "/o/odin3", "--out", "/out")
KEYS = {
    "P": "aaaaaaaaaaaa",
    "Oprime": "bbbbbbbbbbbb",
    "Pprime": "cccccccccccc",
    "O": "dddddddddddd",
}
ALL = ("P", "Pprime", "Oprime", "O", "R", "Oprov")


class FakeInner:
    """A ``run_inner`` that records each argv and answers from ``codes`` (by subcommand, a list
    consumed in order, else 0); ``gate`` and ``table`` print their directory."""

    def __init__(self, codes: dict[str, list[int]] | None = None) -> None:
        self.calls: list[list[str]] = []
        self.codes = codes or {}

    def __call__(self, argv: Sequence[str]) -> int:
        self.calls.append(list(argv))
        queue = self.codes.get(argv[0], [])
        code = queue.pop(0) if queue else 0
        if argv[0] == "table" and code == 0:
            print("/out/tables/20261019-0300-x")
        return code


class ExitRunTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(dir=qor_heavy.HEAVY_DIR, prefix="qor-exit-")
        self.addCleanup(self._tmp.cleanup)
        self.out = Path(self._tmp.name)
        self.lines: list[str] = []

    def ctx(
        self,
        inner: FakeInner,
        *,
        legs: Sequence[str] = ALL,
        lacks: dict[str, str] | None = None,
        candidates: list[str] | None = None,
        fingerprint: str = "fp1",
    ) -> qor_exit.ExitContext:
        return qor_exit.ExitContext(
            out=self.out,
            label="exit-20261019",
            sets=SETS,
            legs=tuple(legs),
            lacks=lacks or {},
            common=COMMON,
            run_inner=inner,
            emit=self.lines.append,
            candidates=lambda: candidates,
            keys=lambda: dict(KEYS),
            fingerprint=fingerprint,
        )

    def test_exit_run_steps(self) -> None:
        """The eight steps in order with the exact argv each builds; the step line precedes each."""
        inner = FakeInner()
        code = qor_exit.run_exit(self.ctx(inner, candidates=["sha", "bgm"]))
        self.assertEqual(code, 0)
        equiv = ",".join(
            d
            for d in SETS.order
            if (d in SETS.gate and d in qor_sched.EQUIV_REQUIRED) or d == "stereovision0"
        )
        self.assertEqual(
            inner.calls,
            [
                [
                    "run",
                    *COMMON,
                    "--leg",
                    "P,Oprime,Pprime,O",
                    "--set",
                    "gate,extra",
                    "--fcheck",
                    "--keep-netlists",
                ],
                ["fcheck", *COMMON, "--leg", "P", "--set", "gate,extra", "--resume"],
                ["equiv", *COMMON, "--designs", equiv, "--resume"],
                ["rerun-check", *COMMON, "--leg", "O", "--set", "gate", "--resume"],
                ["run", *COMMON, "--arch", "earch", "--set", "smoke", "--leg", "P,Pprime,Oprime,O"],
                ["run", *COMMON, "--leg", "P,O", "--designs", "sha,bgm", "--seed", "2"],
                ["run", *COMMON, "--leg", "P,O", "--designs", "sha,bgm", "--seed", "3"],
                [
                    "table",
                    *COMMON,
                    "--legs",
                    "P,Oprime,Pprime,O",
                    "--keys",
                    "P=aaaaaaaaaaaa,Oprime=bbbbbbbbbbbb,Pprime=cccccccccccc,O=dddddddddddd",
                    "--label",
                    "exit-20261019",
                    "--seeds",
                    "2,3",
                ],
                ["gate", "/out/tables/20261019-0300-x"],
            ],
        )
        steps = [line for line in self.lines if line.startswith("exit-run: step")]
        self.assertEqual(
            steps,
            [
                "exit-run: step 1/8 run",
                "exit-run: step 2/8 P fcheck",
                "exit-run: step 3/8 equiv",
                "exit-run: step 4/8 rerun-check",
                "exit-run: step 5/8 EArch smoke",
                "exit-run: step 6/8 seed reruns",
                "exit-run: step 7/8 table",
                "exit-run: step 8/8 gate",
            ],
        )

    def test_exit_run_no_candidates_has_no_seed_runs(self) -> None:
        inner = FakeInner()
        self.assertEqual(qor_exit.run_exit(self.ctx(inner, candidates=[])), 0)
        self.assertEqual(
            [c[0] for c in inner.calls],
            ["run", "fcheck", "equiv", "rerun-check", "run", "table", "gate"],
        )
        self.assertNotIn("--seeds", inner.calls[-2])
        self.assertIn("exit-run: seed reruns: no design is near a bound", self.lines)

    def test_exit_run_resumes_after_a_stop(self) -> None:
        """Step 3 stops short (exit 3, the deadline): a second start repeats nothing of steps 1
        and 2, resumes at step 3, and goes on to 4 to 8."""
        first = FakeInner({"equiv": [3]})
        self.assertEqual(qor_exit.run_exit(self.ctx(first, candidates=[])), 3)
        # no table or gate
        self.assertEqual(
            [c[0] for c in first.calls], ["run", "fcheck", "equiv", "rerun-check", "run"]
        )
        self.assertEqual(
            qor_exit.load_done(self.out, "exit-20261019", "fp1"), {1: 0, 2: 0, 4: 0, 5: 0, 6: 0}
        )
        second = FakeInner()
        self.assertEqual(qor_exit.run_exit(self.ctx(second, candidates=[])), 0)
        self.assertEqual([c[0] for c in second.calls], ["equiv", "table", "gate"])
        self.assertIn("exit-run: step 1/8 run already done (exit 0)", self.lines)

    def test_exit_run_exit_code(self) -> None:
        # a fake gate FAIL: exit 1; a gate INCOMPLETE: 3; PASS: 0
        for gate, expected in (([1], 1), ([3], 3), ([0], 0)):
            self.setUp()
            self.assertEqual(
                qor_exit.run_exit(self.ctx(FakeInner({"gate": gate}), candidates=[])), expected
            )
        # a capped mcml P in step 1 (exit 3): the exit is 3 and no table is written
        self.setUp()
        inner = FakeInner({"run": [3]})
        self.assertEqual(qor_exit.run_exit(self.ctx(inner, candidates=[])), 3)
        self.assertNotIn("table", [c[0] for c in inner.calls])
        # a failed job (exit 1) in step 1 still goes on to the table, and the gate decides
        self.setUp()
        inner = FakeInner({"run": [1], "gate": [1]})
        self.assertEqual(qor_exit.run_exit(self.ctx(inner, candidates=[])), 1)
        self.assertEqual(inner.calls[-1][0], "gate")
        # the disk floor ends the run at once with 5
        self.setUp()
        inner = FakeInner({"fcheck": [5]})
        self.assertEqual(qor_exit.run_exit(self.ctx(inner, candidates=[])), 5)
        self.assertEqual([c[0] for c in inner.calls], ["run", "fcheck"])
        # a table that cannot be made (no complete key): its code, no gate
        self.setUp()
        inner = FakeInner({"table": [1]})
        inner_code = qor_exit.run_exit(self.ctx(inner, candidates=[]))
        self.assertEqual(inner_code, 1)

    def test_failures_outside_the_gate_fail_the_exit(self) -> None:
        """Review H1/M1: a failed EArch smoke job (step 5) or seed job (step 6) is exit 1 even
        when the gate answers PASS; the table is made from this run's keys."""
        for failing in (1, 2):  # the 2nd and 3rd run command: step 5, step 6
            self.setUp()
            inner = FakeInner({"run": [1 if i == failing else 0 for i in range(3)]})
            self.assertEqual(qor_exit.run_exit(self.ctx(inner, candidates=["sha"])), 1, failing)
            table = next(c for c in inner.calls if c[0] == "table")
            self.assertIn("O=dddddddddddd", table[table.index("--keys") + 1])
            self.assertTrue(any("run --rerun" in line for line in self.lines))

    def test_gate_judges_steps_one_to_four(self) -> None:
        """Review N1: a permitted fcheck fail (exit 1 of step 1 or 2, gate PASS) does not fail
        the run; an unpermitted one (gate FAIL) does."""
        for codes in ({"run": [1]}, {"fcheck": [1]}, {"equiv": [1]}, {"rerun-check": [1]}):
            self.setUp()
            self.assertEqual(
                qor_exit.run_exit(self.ctx(FakeInner({**codes, "gate": [0]}), candidates=[])),
                0,
                codes,
            )
            self.setUp()
            self.assertEqual(
                qor_exit.run_exit(self.ctx(FakeInner({**codes, "gate": [1]}), candidates=[])),
                1,
                codes,
            )

    def test_changed_fingerprint_reruns_every_step(self) -> None:
        """Review H2: state recorded for one odin3 build is discarded for another."""
        first = FakeInner({"equiv": [3]})
        qor_exit.run_exit(self.ctx(first, candidates=[], fingerprint="build-A"))
        same = FakeInner()
        qor_exit.run_exit(self.ctx(same, candidates=[], fingerprint="build-A"))
        self.assertEqual(same.calls[0][0], "equiv")  # resumed
        rebuilt = FakeInner()
        self.lines.clear()
        qor_exit.run_exit(self.ctx(rebuilt, candidates=[], fingerprint="build-B"))
        self.assertEqual([c[0] for c in rebuilt.calls][:2], ["run", "fcheck"])
        self.assertTrue(any("for another build or input set" in line for line in self.lines))

    def test_leg_skip_message(self) -> None:
        """O unavailable: the exact skip line, the other legs run."""
        lacks = {"O": "abc", "Oprov": "write_blif --name-style provenance"}
        inner = FakeInner()
        legs = tuple(leg for leg in ALL if leg not in lacks)
        qor_exit.run_exit(self.ctx(inner, legs=legs, lacks=lacks, candidates=[]))
        self.assertIn("O skipped: needs pass 'abc'", self.lines)
        self.assertNotIn(
            "Oprov skipped: needs pass 'write_blif --name-style provenance'", self.lines
        )  # Oprov is not an exit-run leg
        self.assertEqual(inner.calls[0][inner.calls[0].index("--leg") + 1], "P,Oprime,Pprime")
        smoke = inner.calls[4]
        self.assertEqual(smoke[smoke.index("--leg") + 1], "P,Pprime,Oprime")

    def test_state_file_is_atomic_and_tolerant(self) -> None:
        self.assertEqual(qor_exit.load_done(self.out, "x"), {})
        qor_exit.save_done(self.out, "x", {2: 0, 1: 1})
        self.assertEqual(qor_exit.load_done(self.out, "x"), {1: 1, 2: 0})
        qor_exit.state_path(self.out, "x").write_text("not json")
        self.assertEqual(qor_exit.load_done(self.out, "x"), {})


class ExitRunDeadlineTest(RunnerTestBase):
    def test_exit_run_deadline(self) -> None:
        """Started at 01:00 on a fake clock: a job expected to pass 06:00 is not started, with the
        nightly's E-QH10 text and ``exit-run`` in place of ``nightly``; the next invocation (the
        job is not cached) runs it."""
        expected = {"sha": 120, "diffeq1": 120, "bgm": 100}
        clock = FakeClock()
        deadline = qor_nightly.nightly_deadline(datetime(2026, 10, 19, 1, 0), 5.0)
        vtr_qor.ADMISSION = vtr_qor.Admission(
            (deadline - datetime(2026, 10, 19, 1, 0)).total_seconds(),
            "06:00",
            lambda job: expected[job.design] * 60.0,
            begun=clock.now,
            owner="exit-run",
        )
        self.addCleanup(setattr, vtr_qor, "ADMISSION", None)
        opts = self.opts(clock=clock, sleep=clock.sleep)
        with mock.patch.object(time, "monotonic", clock):
            vtr_qor._apply_admission(opts)
        self.assertEqual((opts.deadline_text, opts.deadline_owner), ("06:00", "exit-run"))

        def finish(job: qor_sched.Job, start: float) -> dict[str, object]:
            clock.now += expected[job.design] * 60.0
            return {"schema": 1, "leg": job.leg, "routed": {"vpr_status": "success"}}

        jobs = [self.job(d, [{}]) for d in expected]
        summary, lines = self.run_jobs(jobs, opts, finish)
        self.assertEqual([r.status for r in summary.results], ["ok", "ok", "skipped"])
        self.assertIn(
            "vtr-qor: exit-run: P bgm not started: expected 100 min would pass 06:00", lines
        )


def read_task(path: Path) -> dict[str, list[str]]:
    """A VTR task config (``key=value`` lines, ``#`` comments) as VTR's ``load_task_config``
    reads it: a repeated key keeps every value, in order."""
    values: dict[str, list[str]] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            key, _, value = line.partition("=")
            values.setdefault(key, []).append(value)
    return values


class TaskConfigTest(unittest.TestCase):
    """3F (folded into 3H): the committed ``tests/vtr/tasks`` configs agree with the harness."""

    def test_task_configs(self) -> None:
        expected = {"qor_p": list(SETS.order[:3]), "qor_gate": list(SETS.order)}
        self.assertEqual(sorted(p.name for p in TASKS.iterdir()), sorted(expected))
        for name, designs in expected.items():
            task = read_task(TASKS / name / "config" / "config.txt")
            self.assertEqual(task["circuit_list_add"], [f"{d}.v" for d in designs], name)
            self.assertEqual(task["arch_list_add"], [f"{qor_legs.ARCH_BASES['k6']}.xml"])
            # the harness's common parameters, and nothing machine specific
            self.assertEqual(task["script_params"], [" ".join(qor_legs.QOR_CHAIN_PARAMS)])
            self.assertEqual(
                (task["circuits_dir"], task["archs_dir"]), (["benchmarks/verilog"], ["arch/timing"])
            )
            self.assertEqual(
                (task["parse_file"], task["qor_parse_file"]),
                (["vpr_standard.txt"], ["qor_standard.txt"]),
            )
            self.assertNotIn("/home/", (TASKS / name / "config" / "config.txt").read_text())

    def test_qor_gate_is_the_p_leg_set(self) -> None:
        task = read_task(TASKS / "qor_gate" / "config" / "config.txt")
        names = [d.removesuffix(".v") for d in task["circuit_list_add"]]
        self.assertEqual(set(names), set(SETS.gate) | set(SETS.extra) | set(SETS.offload))

    def test_the_default_task_is_qor_p(self) -> None:
        self.assertEqual(qor_nightly.DEFAULT_TASK, TASKS / "qor_p")

    def test_task_files_exist_in_vtr(self) -> None:
        """When a VTR checkout is at hand: the parse files and designs the configs name exist."""
        vtr = Path.home() / "odin3-ws" / "external" / "vtr-verilog-to-routing" / "vtr_flow"
        if not vtr.is_dir():
            self.skipTest("no VTR checkout")
        for name in ("qor_p", "qor_gate"):
            task = read_task(TASKS / name / "config" / "config.txt")
            self.assertTrue((vtr / "parse" / "parse_config" / task["parse_file"][0]).is_file())
            self.assertTrue((vtr / "parse" / "qor_config" / task["qor_parse_file"][0]).is_file())
            for circuit in task["circuit_list_add"]:
                self.assertTrue((vtr / "benchmarks" / "verilog" / circuit).is_file(), circuit)
            self.assertTrue((vtr / "arch" / "timing" / task["arch_list_add"][0]).is_file())

    def test_vtr_loads_the_configs(self) -> None:
        """VTR's own ``load_task_config`` (read only, no flow runs) accepts both configs."""
        root = Path.home() / "odin3-ws" / "external" / "vtr-verilog-to-routing"
        python = root / ".venv" / "bin" / "python"
        if not python.is_file():
            self.skipTest("no VTR venv")
        snippet = (
            "import sys\nsys.path.insert(0, sys.argv[1])\n"
            "from vtr.task import load_task_config\n"
            "c = load_task_config(sys.argv[2])\n"
            "print(len(c.circuits), ' '.join(c.script_params))\n"
        )
        for name, count in (("qor_p", 3), ("qor_gate", len(SETS.order))):
            proc = subprocess.run(
                [
                    str(python),
                    "-I",
                    "-B",
                    "-c",
                    snippet,
                    str(root / "vtr_flow" / "scripts" / "python_libs"),
                    str(TASKS / name / "config" / "config.txt"),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(
                proc.stdout.strip(), f"{count} -track_memory_usage -crit_path_router_iterations 100"
            )


if __name__ == "__main__":
    unittest.main()

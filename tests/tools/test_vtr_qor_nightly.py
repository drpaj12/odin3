"""Tests for the nightly, check-lib and crosscheck (3H plan Task 13): tools/vtr-qor/qor_nightly.py
and the ``nightly``, ``check-lib`` and ``crosscheck`` subcommands of vtr_qor.py.

The clock is fake, jobs are fakejob.py runs, the git repositories are temporary ones, and the
inner subcommands and child tools are recorded, never started.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import subprocess
import tempfile
import unittest
from datetime import date, datetime
from pathlib import Path
from typing import Any
from unittest import mock

import qor_heavy
import qor_nightly
import qor_sched
import vtr_qor
from qor_nightly import ROTATION, Slot
from qor_table import Run

from .helpers import REPO_ROOT
from .test_vtr_qor_cli import K6, CliTestBase
from .test_vtr_qor_sched import FakeClock, RunnerTestBase

SETS = qor_sched.load_sets()
MONDAY, SATURDAY, SUNDAY = date(2026, 10, 19), date(2026, 10, 17), date(2026, 10, 18)


def git(repo: Path, *args: str) -> str:
    proc = subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t",
                           "-c", "commit.gpgsign=false", *args],
                          capture_output=True, text=True, check=True)
    return proc.stdout.strip()


class DeadlineTest(RunnerTestBase):
    def test_deadline_function(self) -> None:
        at = datetime(2026, 10, 18, 1, 0)
        self.assertEqual(qor_nightly.nightly_deadline(at, 5.0), datetime(2026, 10, 18, 6, 0))
        self.assertEqual(qor_nightly.nightly_deadline(at, 2.0), datetime(2026, 10, 18, 3, 0))
        late = datetime(2026, 10, 18, 14, 0)
        self.assertEqual(qor_nightly.nightly_deadline(late, 4.0), datetime(2026, 10, 18, 18, 0))
        self.assertEqual(qor_nightly.nightly_deadline(late, 5.0), datetime(2026, 10, 18, 19, 0))
        self.assertEqual(qor_nightly.nightly_deadline(late, 20.0), datetime(2026, 10, 19, 6, 0))
        six = datetime(2026, 10, 18, 6, 0)  # at 06:00 sharp the next deadline is tomorrow's
        self.assertEqual(qor_nightly.nightly_deadline(six, 24.0), datetime(2026, 10, 19, 6, 0))

    def test_nightly_deadline(self) -> None:
        """Started at 01:00 with jobs of 120, 120, 100 and 30 expected minutes: the first two
        start, the third is refused at 05:00 with the exact E-QH10 text, the fourth still
        starts."""
        expected = {"sha": 120, "diffeq1": 120, "bgm": 100, "mkPktMerge": 30}
        clock = FakeClock()
        deadline = qor_nightly.nightly_deadline(datetime(2026, 10, 18, 1, 0), 5.0)
        opts = self.opts(clock=clock, sleep=clock.sleep)
        opts.deadline = clock.now + (deadline - datetime(2026, 10, 18, 1, 0)).total_seconds()
        opts.deadline_text = deadline.strftime("%H:%M")
        opts.expected_s = lambda job: expected[job.design] * 60.0

        def finish(job: qor_sched.Job, start: float) -> dict[str, object]:
            clock.now += expected[job.design] * 60.0  # the job took its expected time
            return {"schema": 1, "leg": job.leg, "routed": {"vpr_status": "success"}}

        jobs = [self.job(d, [{}]) for d in expected]
        summary, lines = self.run_jobs(jobs, opts, finish)
        self.assertEqual([r.status for r in summary.results], ["ok", "ok", "skipped", "ok"])
        self.assertIn("vtr-qor: nightly: P bgm not started: expected 100 min would pass 06:00",
                      lines)
        self.assertEqual(summary.exit_code, 0)  # the refused line is exit 0 for that line alone

    def test_expected_minutes(self) -> None:
        job = self.job("sha", [{}])
        history = self.tmp / "history"
        self.assertEqual(qor_nightly.expected_minutes(job, history, SETS),
                         SETS.designs["sha"].vtr_flow_time_s * 1.5 / 60.0)

        def record(leg: str, key: str, **fields: object) -> None:
            path = history / "aaaaaaaaaaaa" / K6 / leg / key / "sha" / "metrics.json"
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps({"status": "ok", **fields}))

        record("P", "k1", elapsed_s=600.0)
        self.assertEqual(qor_nightly.expected_minutes(job, history, SETS), 10.0)
        odin3_job = dataclasses.replace(job, leg="O")
        flow = SETS.designs["sha"].vtr_flow_time_s * 1.5
        self.assertEqual(qor_nightly.expected_minutes(odin3_job, history, SETS), flow / 60.0)
        record("O", "k2", cost={"frontend_wall_s": 60.0}, status="ok")
        (history / "aaaaaaaaaaaa" / K6 / "O" / "k2" / "sha" / "metrics.json").write_text(
            json.dumps({"status": "ok", "cost": {"frontend_wall_s": 60.0}}))
        self.assertEqual(qor_nightly.expected_minutes(odin3_job, history, SETS),
                         (flow + 60.0) / 60.0)


class PlanTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(dir=qor_heavy.HEAVY_DIR)
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        for rel in ("src/backends/blif/writer.c", "src/passes/abc/run.c", "src/ir/x.c"):
            (self.repo / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.repo / rel).write_text("// first\n")
        git(self.repo, "init", "-q")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "one")
        self.first = git(self.repo, "rev-parse", "HEAD")

    def commit(self, rel: str) -> None:
        with (self.repo / rel).open("a") as handle:
            handle.write("// more\n")
        git(self.repo, "commit", "-q", "-am", f"edit {rel}")

    def state(self, **fields: object) -> dict[str, object]:
        state = qor_nightly.default_state()
        state.update(last_sweep_commit=self.first, last_3d_commit=self.first, **fields)
        return state

    def test_3d_sweep_night(self) -> None:
        """A changed src/passes/abc/run.c: the five 3D commands in order, each started with
        descriptor 9 passed, and no group B slot tonight."""
        self.commit("src/passes/abc/run.c")
        plan = qor_nightly.plan_night(self.state(), self.repo, MONDAY)
        self.assertEqual((plan.sweep_3d, plan.sweep_3e, plan.slot), (True, False, None))
        tools = self.repo / "tools" / "abc-check"
        golden = Path(self._tmp.name) / "golden" / K6
        for name in ("abc-equiv", "abc-diff", "abc-determinism"):
            (tools / name).parent.mkdir(parents=True, exist_ok=True)
            (tools / name).write_text("#!/bin/sh\n")
        for design in ("ch_intrinsics", "mcml", "LU32PEEng", "sha"):
            path = golden / "vtr" / design / f"{design}.parmys.blif"
            path.parent.mkdir(parents=True)
            path.write_text("")
        commands = qor_nightly.sweep_3d_commands(self.repo, Path("/o/odin3"), golden, SETS)
        self.assertEqual([Path(c[0]).name for c in commands],
                         ["abc-equiv", "abc-diff", "abc-determinism", "abc-determinism",
                          "abc-determinism"])
        self.assertEqual([c[c.index("--mode") + 1] for c in commands[2:]], ["a", "c", "d"])
        big = " ".join(" ".join(c) for c in commands[:4])
        self.assertIn("ch_intrinsics.parmys.blif", big)
        self.assertNotIn("mcml", big)  # the large designs are the controller's (3D §13.9)
        self.assertNotIn("LU32PEEng", big)
        self.assertNotIn("/vtr/", " ".join(commands[4]))  # --mode d: the micros only
        seen: list[tuple[list[str], tuple[int, ...]]] = []

        def fake_run(argv: list[str], **kwargs: Any) -> subprocess.CompletedProcess[str]:
            seen.append((argv, kwargs["pass_fds"]))
            return subprocess.CompletedProcess(argv, 0)

        lines: list[str] = []
        with mock.patch.object(qor_sched, "lock_fds", return_value=(9,)), \
                mock.patch.object(subprocess, "run", fake_run):
            summary = qor_nightly.run_sweeps(commands, lines.append, "3D sweeps")
        self.assertEqual([argv for argv, _ in seen], commands)
        self.assertEqual({fds for _, fds in seen}, {(9,)})
        self.assertEqual(summary[0], "abc-equiv: exit 0")

    def test_absent_tools_are_skipped(self) -> None:
        lines: list[str] = []
        out = qor_nightly.run_sweeps([[str(self.repo / "tools/abc-check/abc-diff")]],
                                     lines.append, "3D sweeps")
        self.assertEqual(lines, ["nightly: 3D sweeps skipped (abc-check/abc-diff absent)"])
        self.assertEqual(out, ["abc-diff: skipped (absent)"])

    def test_sweep_night(self) -> None:
        """A changed src/backends/blif/writer.c: step 5 runs, step 4 is skipped with the rotation
        unchanged. No change and not Saturday: no sweep."""
        self.commit("src/backends/blif/writer.c")
        state = self.state(next_slot=3)
        plan = qor_nightly.plan_night(state, self.repo, MONDAY)
        self.assertEqual((plan.sweep_3e, plan.sweep_3d, plan.slot), (True, False, None))
        self.assertEqual(state["next_slot"], 3)
        self.assertEqual(MONDAY.weekday(), 0)
        quiet = qor_nightly.plan_night(self.state(next_slot=3), self.repo, MONDAY)
        head = git(self.repo, "rev-parse", "HEAD")
        again = qor_nightly.plan_night({**self.state(next_slot=3), "last_sweep_commit": head},
                                       self.repo, MONDAY)
        self.assertEqual((quiet.sweep_3e, again.sweep_3e), (True, False))
        self.assertEqual(again.slot, ROTATION[2])
        saturday = qor_nightly.plan_night({**self.state(), "last_sweep_commit": head},
                                          self.repo, SATURDAY)
        self.assertEqual((saturday.sweep_3e, saturday.slot), (True, None))
        sunday = qor_nightly.plan_night(self.state(), self.repo, SUNDAY)
        self.assertEqual((sunday.sweep_3d, sunday.slot), (True, None))
        golden = Path("/g") / K6
        commands = qor_nightly.sweep_3e_commands(self.repo, golden)
        self.assertEqual([Path(c[0]).name for c in commands], ["blif-roundtrip",
                                                               "vtr-read-check"])
        self.assertEqual(commands[0][1], "--vtr")

    def test_no_recorded_commit_is_no_change(self) -> None:
        plan = qor_nightly.plan_night(qor_nightly.default_state(), self.repo, MONDAY)
        self.assertEqual((plan.sweep_3e, plan.sweep_3d, plan.slot), (False, False, ROTATION[0]))
        self.assertTrue(qor_nightly.changed_since(self.repo, "0" * 40, ("src",)))  # unresolvable

    def test_rotation_advance_and_starvation(self) -> None:
        state = self.state(next_slot=3)
        qor_nightly.settle_slot(state, ROTATION[2], ran=True, today=MONDAY)
        self.assertEqual((state["next_slot"], state["last_admitted"]), (4, "2026-10-19"))
        plan = qor_nightly.plan_night(state, self.repo, MONDAY)
        self.assertEqual((plan.slot, plan.starved_first), (ROTATION[3], False))
        qor_nightly.settle_slot(state, ROTATION[3], ran=False, today=MONDAY)  # refused
        self.assertEqual(state["next_slot"], 4)  # a slot advances only when it ran
        plan = qor_nightly.plan_night(state, self.repo, MONDAY)
        self.assertEqual((plan.slot, plan.starved_first, plan.starved_nights),
                         (ROTATION[3], True, 1))  # step 4 before step 3
        qor_nightly.settle_slot(state, ROTATION[3], ran=False, today=MONDAY)
        line = qor_nightly.starved_line(ROTATION[3], 2)
        self.assertEqual(line, "vtr-qor: nightly: group B slot 4 (mcml O′) starved for "
                               "2 nights")
        self.assertTrue(qor_nightly.summary_text([line], ["1 doctor: exit 0"], []
                                                 ).startswith(line + "\n"))
        qor_nightly.settle_slot(state, ROTATION[3], ran=True, today=MONDAY)
        self.assertEqual((state["next_slot"], state["starved"]), (5, {}))
        wrap = self.state(next_slot=8)
        qor_nightly.settle_slot(wrap, ROTATION[7], ran=True, today=MONDAY)
        self.assertEqual(wrap["next_slot"], 1)
        self.assertEqual([s.number for s in ROTATION], list(range(1, 9)))
        self.assertIsNone(qor_nightly.available_slot(ROTATION[0], {"P"}))
        part = qor_nightly.available_slot(ROTATION[0], {"P", "O", "Pprime"})
        self.assertEqual(part, Slot(1, "LU8PEEng", ("O", "Pprime"), True, False))

    def test_state_file(self) -> None:
        path = Path(self._tmp.name) / "nightly" / "rotation.json"
        self.assertEqual(qor_nightly.load_state(path), qor_nightly.default_state())
        state = self.state(next_slot=5)
        qor_nightly.save_state(path, state)
        self.assertEqual(qor_nightly.load_state(path), state)
        path.write_text('{"next_slot": 99, "starved": 3}')
        loaded = qor_nightly.load_state(path)
        self.assertEqual((loaded["next_slot"], loaded["starved"]), (1, {}))


def make_run(leg: str, design: str, status: str = "ok", clb: float = 100.0) -> Run:
    metrics = {"leg": leg, "design": design, "status": status,
               "packed": {"num_clb": clb}, "routed": {"min_chan_width": 60}}
    return Run(leg=leg, design=design, arch=K6, key12="0" * 12, status=status, gating=True,
               metrics=metrics)


class SummaryTest(unittest.TestCase):
    def test_summary_diff(self) -> None:
        before = [make_run("O", "sha"), make_run("O", "bgm"), make_run("P", "sha"),
                  make_run("Pprime", "sha")]
        after = [make_run("O", "sha", clb=102.5), make_run("O", "bgm", clb=101.9),
                 make_run("P", "sha", status="failed"), make_run("Pprime", "sha")]
        lines = qor_nightly.diff_runs(before, after, ("num_clb", "min_chan_width"))
        self.assertEqual(lines, ["O sha: num_clb 100 -> 102.5 (+2.5%)",
                                 "P sha: status ok -> failed"])

    def test_previous_table(self) -> None:
        with tempfile.TemporaryDirectory(dir=qor_heavy.HEAVY_DIR) as name:
            tables = Path(name)
            for stamp in ("20261016-0100-nightly-20261016", "20261017-0100-nightly-20261017",
                          "20261017-0900-other"):
                (tables / stamp).mkdir()
            current = tables / "20261018-0100-nightly-20261018"
            self.assertEqual(qor_nightly.previous_table(tables, "nightly-", current),
                             tables / "20261017-0100-nightly-20261017")
            self.assertIsNone(qor_nightly.previous_table(
                tables, "nightly-", tables / "20261016-0100-nightly-20261016"))


class UnitFileTest(unittest.TestCase):
    def test_unit_files(self) -> None:
        directory = REPO_ROOT / "tools" / "vtr-qor" / "systemd"
        service = (directory / "odin3-vtr-qor-nightly.service").read_text().splitlines()
        timer = (directory / "odin3-vtr-qor-nightly.timer").read_text().splitlines()
        for line in ("Type=exec", "Environment=ODIN3_HEAVY_OK=1",
                     "ExecStart=%h/odin3-ws/odin3/tools/heavy-run "
                     "%h/odin3-ws/odin3/tools/vtr-qor/vtr-qor nightly"):
            self.assertIn(line, service)
        for line in ("OnCalendar=*-*-* 01:00", "Persistent=false", "WantedBy=timers.target"):
            self.assertIn(line, timer)
        header = "\n".join(service[:6])
        self.assertIn("systemctl --user daemon-reload && systemctl --user enable --now "
                      "odin3-vtr-qor-nightly.timer", header)
        self.assertIn("systemctl --user disable --now odin3-vtr-qor-nightly.timer", header)


class CliTest(CliTestBase):
    def test_nightly_requires_heavy_run(self) -> None:
        """Outside a heavy run: exit 2 with require_heavy's message, and no step runs."""
        ran: list[list[str]] = []

        def record(argv: Any) -> int:
            ran.append(list(argv))
            return 0

        with mock.patch.dict(os.environ, {"ODIN3_HEAVY_SLICE": ""}), \
                mock.patch.object(vtr_qor, "run_inner", record):
            code, _, err = self.cli("nightly")
        self.assertEqual((code, err), (2, f"vtr-qor: {qor_heavy.MSG_NOT_INSIDE}\n"))
        self.assertEqual(ran, [])

    def test_check_lib_argv(self) -> None:
        argv = qor_nightly.check_lib_command(Path("/r"), Path("/v"))
        self.assertEqual(argv, [
            "/r/tools/vtr-models/vtr-models", "check-lib", "/r/lib/vtr.o3lib", "--models",
            "/r/tests/vtr/arch/k6_frac_N10_frac_chain_mem32K_40nm.models.json",
            "--vtr-root", "/v"])
        with mock.patch.object(subprocess, "run") as run:
            run.return_value = subprocess.CompletedProcess([], 3)
            code, _, _ = self.cli("check-lib")
        self.assertEqual(code, 3)
        self.assertEqual(run.call_args.args[0],
                         qor_nightly.check_lib_command(self.repo, self.vtr))

    def task_results(self, num_clb: str, runtime: str) -> None:
        (self.vtr / "task-results").write_text(
            "circuit\tvpr_status\tnum_clb\tvpr_flow_runtime\tmax_vpr_mem\t\n"
            f"ch_intrinsics.v\tsuccess\t{num_clb}\t{runtime}\t99999\t\n")

    def test_crosscheck_compare(self) -> None:
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        task = self.tmp / "task"
        task.mkdir()
        self.task_results("12", "123.4")  # only time keys differ from the harness's
        code, _, err = self.cli("crosscheck", "--task", str(task))
        self.assertEqual((code, err), (0, ""))
        self.task_results("13", "5")
        code, _, err = self.cli("crosscheck", "--task", str(task))
        self.assertEqual(code, 1)
        self.assertEqual(err, "vtr-qor: crosscheck: ch_intrinsics num_clb: task 13, harness 12\n")

    def test_crosscheck_rows(self) -> None:
        row = {"circuit": "a.v", "num_clb": "1", "vpr_flow_runtime": "9", "max_vpr_mem": "1",
               "total_time": "3"}
        self.assertEqual(qor_nightly.crosscheck_lines("a", row, {"num_clb": "1"}), [])
        self.assertEqual(qor_nightly.crosscheck_lines("a", {"num_clb": "1"}, {}),
                         ["vtr-qor: crosscheck: a num_clb: task 1, harness (absent)"])
        self.assertTrue(qor_nightly.is_time_key("pack_time"))
        self.assertTrue(qor_nightly.is_time_key("max_vpr_mem"))
        self.assertFalse(qor_nightly.is_time_key("num_clb"))


class NightRunTest(CliTestBase):
    """The night's steps with the inner subcommands recorded (never started)."""

    def setUp(self) -> None:
        super().setUp()
        self.calls_made: list[list[str]] = []
        self.refuse = False
        self.log = self.out / "harness.log"
        self.out.mkdir(exist_ok=True)

    def inner(self, argv: Any) -> int:
        self.calls_made.append(list(argv))
        if self.refuse and argv[0] == "run" and "mcml" in argv:
            with self.log.open("a") as handle:
                handle.write("vtr-qor: nightly: Oprime mcml not started: expected 99 min would "
                             "pass 06:00\n")
        return 0

    def night(self, state: dict[str, object], plan: qor_nightly.NightPlan, slot: int | None = None
              ) -> tuple[int, Path]:
        repo = self.tmp / "nightrepo"
        if not repo.exists():
            (repo / "src").mkdir(parents=True)
            (repo / "src" / "a.c").write_text("x\n")
            git(repo, "init", "-q")
            git(repo, "add", "-A")
            git(repo, "commit", "-q", "-m", "one")
        ctx = qor_nightly.NightContext(
            out=self.out, repo=repo, odin3=self.odin3, vtr_root=self.vtr,
            golden_k6=self.golden / K6, sets=SETS, legs=("P", "Pprime", "Oprime", "O", "R"),
            common=("--out", str(self.out)), today=MONDAY, run_inner=self.inner,
            emit=lambda m: None, refused=vtr_qor._refused_counter(self.out))
        args = argparse.Namespace(slot=slot)
        with mock.patch.object(vtr_qor, "_nightly_table", return_value=None):
            code = vtr_qor._run_night(args, ctx, state, plan)
        return code, self.out / "nightly" / "20261019" / "summary.md"

    def test_steps_and_starvation(self) -> None:
        state = qor_nightly.default_state()
        state.update(next_slot=4, starved={"4": 1})
        plan = qor_nightly.NightPlan(ROTATION[3], True, False, False, 1)
        self.refuse = True
        code, summary = self.night(state, plan)
        self.assertEqual(code, 0)
        runs = [c for c in self.calls_made if c[0] == "run"]
        # The starved slot (mcml O') runs before group A (step 3), then step 3 follows.
        self.assertIn("mcml", runs[1])  # [0] is the EArch smoke
        self.assertIn("--designs", runs[2])
        self.assertNotIn("mcml", runs[2])
        self.assertEqual(self.calls_made[0][0], "doctor")
        self.assertEqual((state["next_slot"], state["starved"]), (4, {"4": 2}))
        self.assertTrue(summary.read_text().startswith(
            "vtr-qor: nightly: group B slot 4 (mcml O′) starved for 2 nights\n"))
        # Two refused nights: group A runs no equiv; an admitted slot advances and clears.
        self.calls_made.clear()
        plan = qor_nightly.NightPlan(ROTATION[3], True, False, False, 2)
        self.refuse = False
        code, _ = self.night(state, plan)
        self.assertEqual(code, 0)
        self.assertFalse(any(c[0] == "equiv" for c in self.calls_made))
        self.assertEqual((state["next_slot"], state["starved"]), (5, {}))

    def test_sweep_night_skips_step_4(self) -> None:
        state = qor_nightly.default_state()
        state["next_slot"] = 3
        plan = qor_nightly.NightPlan(None, False, True, False, 0)
        with mock.patch.object(qor_nightly, "run_sweeps", return_value=["swept"]) as sweeps:
            code, summary = self.night(state, plan)
        self.assertEqual(code, 0)
        sweeps.assert_called_once()
        self.assertEqual(state["next_slot"], 3)  # the rotation did not move
        self.assertFalse(any("mcml" in c or "LU8PEEng" in c for c in self.calls_made))
        self.assertIn("swept", summary.read_text())
        self.assertTrue(state["last_sweep_commit"])


if __name__ == "__main__":
    unittest.main()

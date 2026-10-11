"""Tests for tools/heavy-run, tools/build-locked and odin3_add_sweep_test (2E plan point (p), ruling
C1), over fake flock, systemctl and systemd-run scripts on PATH that log their argv.

The live probe at the end runs the real heavy-run (`true`) and reads the slice's cap back; it is
skipped without a systemd --user manager, and when the heavy lock stays busy for 10 s.
"""

from __future__ import annotations

import os
import shutil
import signal
import stat
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from . import helpers

HEAVY_RUN = helpers.REPO_ROOT / "tools" / "heavy-run"
BUILD_LOCKED = helpers.REPO_ROOT / "tools" / "build-locked"
DEFAULT_DIR = Path("/tmp/claude-1000")
BASH = shutil.which("bash") or "/bin/bash"
SKIP = 77

# Each fake appends one line per call to $FAKE_LOG: its name and argv, then key=value facts.
FAKE_SYSTEMCTL = """#!{bash}
echo "systemctl $*" >>"$FAKE_LOG"
exit "${{FAKE_SYSTEMCTL_RC:-0}}"
"""
# flock: heavy-run calls `flock -x -w N 9` on its lock descriptor; build-locked execs
# `flock -s LOCK cmake --build ...` (logged, not run).
FAKE_FLOCK = """#!{bash}
dir=${{ODIN3_HEAVY_DIR:-/tmp/claude-1000}}
flag=no
[[ -e $dir/odin3-heavy.queued.$PPID ]] && flag=yes
fd9=$(readlink /proc/$$/fd/9 2>/dev/null || echo none)
echo "flock $* | flag=$flag fd9=$fd9" >>"$FAKE_LOG"
exit "${{FAKE_FLOCK_RC:-0}}"
"""
FAKE_SYSTEMD_RUN = """#!{bash}
dir=${{ODIN3_HEAVY_DIR:-/tmp/claude-1000}}
flag=no
[[ -n $(ls "$dir"/odin3-heavy.queued.$PPID 2>/dev/null) ]] && flag=yes
echo "systemd-run $* | slice=${{ODIN3_HEAVY_SLICE:-}} flag=$flag" >>"$FAKE_LOG"
while [[ $1 == -* ]]; do shift; done
exec "$@"
"""


def _write_exec(path: Path, text: str) -> None:
    path.write_text(text.format(bash=BASH))
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class FakeTools(unittest.TestCase):
    """A temporary bin directory of fakes put first on PATH, and their log."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        _write_exec(self.bin / "systemctl", FAKE_SYSTEMCTL)
        _write_exec(self.bin / "flock", FAKE_FLOCK)
        _write_exec(self.bin / "systemd-run", FAKE_SYSTEMD_RUN)
        self.log = self.dir / "fake.log"
        self.env = {
            k: v for k, v in os.environ.items() if not k.startswith(("ODIN3_HEAVY", "FAKE_"))
        }
        self.env["PATH"] = f"{self.bin}:{os.environ.get('PATH', '/usr/bin:/bin')}"
        self.env["FAKE_LOG"] = str(self.log)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def calls(self) -> list[str]:
        return self.log.read_text().splitlines() if self.log.exists() else []

    def run_tool(self, tool: Path, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([BASH, str(tool), *args], env={**self.env, **env},
                              capture_output=True, text=True, timeout=60, check=False)


class HeavyRunTest(FakeTools):
    def test_runs_in_the_slice_under_the_lock(self) -> None:
        run = self.run_tool(HEAVY_RUN, "echo", "hi", ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "hi\n")
        self.assertEqual(self.calls(), [
            "systemctl --user set-property --runtime odin3-heavy.slice MemoryMax=6G "
            "MemorySwapMax=0",
            # the flag is written before the wait, on the lock's descriptor
            f"flock -x -w 14400 9 | flag=yes fd9={DEFAULT_DIR}/odin3-heavy.lock",
            # and removed once the lock is held
            "systemd-run --user --scope -q --slice=odin3-heavy.slice echo hi"
            " | slice=odin3-heavy.slice flag=no",
        ])

    def test_the_flag_names_the_pid(self) -> None:
        heavy_dir = self.dir / "heavy"
        script = self.dir / "show-flag"
        _write_exec(script, '#!{bash}\nls "$ODIN3_HEAVY_DIR" >>"$FAKE_LOG"\n')
        _write_exec(self.bin / "flock", '#!{bash}\nls "$ODIN3_HEAVY_DIR" >>"$FAKE_LOG"\n'
                    'cat "$ODIN3_HEAVY_DIR"/odin3-heavy.queued.* >>"$FAKE_LOG"\n')
        run = self.run_tool(HEAVY_RUN, str(script), ODIN3_HEAVY_OK="1",
                            ODIN3_HEAVY_DIR=str(heavy_dir))
        self.assertEqual(run.returncode, 0, run.stderr)
        calls = self.calls()
        flag = next(line for line in calls if line.startswith("odin3-heavy.queued."))
        self.assertEqual(calls[calls.index(flag) + 1], flag.rsplit(".", 1)[1])  # its PID inside
        self.assertEqual(calls[-1], "odin3-heavy.lock")  # the job runs without the flag
        self.assertEqual(sorted(p.name for p in heavy_dir.iterdir()), ["odin3-heavy.lock"])

    def test_lock_timeout_skips(self) -> None:
        heavy_dir = self.dir / "heavy"
        run = self.run_tool(HEAVY_RUN, "echo", "hi", ODIN3_HEAVY_OK="1", FAKE_FLOCK_RC="1",
                            ODIN3_HEAVY_DIR=str(heavy_dir))
        self.assertEqual(run.returncode, SKIP)
        self.assertEqual(run.stderr, "heavy-run: lock busy for 14400 s; skipped\n")
        self.assertFalse(any(line.startswith("systemd-run") for line in self.calls()))
        self.assertEqual([p.name for p in heavy_dir.iterdir()], ["odin3-heavy.lock"])

    def test_wait_is_odin3_heavy_wait(self) -> None:
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", ODIN3_HEAVY_WAIT="5",
                            FAKE_FLOCK_RC="1", ODIN3_HEAVY_DIR=str(self.dir / "heavy"))
        self.assertEqual(run.returncode, SKIP)
        self.assertIn("lock busy for 5 s", run.stderr)
        self.assertTrue(self.calls()[1].startswith("flock -x -w 5 9 "))

    def test_job_status_is_returned(self) -> None:
        run = self.run_tool(HEAVY_RUN, "sh", "-c", "exit 1", ODIN3_HEAVY_OK="1",
                            ODIN3_HEAVY_DIR=str(self.dir / "heavy"))
        self.assertEqual(run.returncode, 1)  # a job's exit 1 is not a busy lock

    def test_xl_slice(self) -> None:
        heavy_dir = str(self.dir / "heavy")
        run = self.run_tool(HEAVY_RUN, "--xl", "true", ODIN3_HEAVY_OK="1",
                            ODIN3_HEAVY_DIR=heavy_dir)
        self.assertEqual(run.returncode, SKIP)
        self.assertIn("ODIN3_HEAVY_XL_OK", run.stderr)
        self.assertEqual(self.calls(), [])
        run = self.run_tool(HEAVY_RUN, "--xl", "true", ODIN3_HEAVY_OK="1", ODIN3_HEAVY_XL_OK="1",
                            ODIN3_HEAVY_DIR=heavy_dir)
        self.assertEqual(run.returncode, 0, run.stderr)
        calls = self.calls()
        self.assertEqual(calls[0], "systemctl --user set-property --runtime odin3-heavyxl.slice "
                                   "MemoryMax=9G MemorySwapMax=0")
        self.assertTrue(calls[2].startswith(
            "systemd-run --user --scope -q --slice=odin3-heavyxl.slice true | "
            "slice=odin3-heavyxl.slice"))

    def test_skips_without_ok(self) -> None:
        run = self.run_tool(HEAVY_RUN, "true")
        self.assertEqual(run.returncode, SKIP)
        self.assertEqual(run.stderr, "heavy-run: ODIN3_HEAVY_OK is not 1; skipped\n")
        self.assertEqual(self.calls(), [])

    def test_skips_without_systemd_run(self) -> None:
        empty = self.dir / "empty"
        empty.mkdir()
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", PATH=str(empty))
        self.assertEqual(run.returncode, SKIP)
        self.assertEqual(run.stderr, "heavy-run: systemd-run not found; skipped\n")

    def test_skips_without_a_user_manager(self) -> None:
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", FAKE_SYSTEMCTL_RC="1",
                            ODIN3_HEAVY_DIR=str(self.dir / "heavy"))
        self.assertEqual(run.returncode, SKIP)
        self.assertIn("cannot set up odin3-heavy.slice", run.stderr)

    def test_usage(self) -> None:
        run = self.run_tool(HEAVY_RUN, ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, 2)


class BuildLockedTest(FakeTools):
    def test_waits_for_a_live_queued_heavy_run(self) -> None:
        heavy_dir = self.dir / "heavy"
        heavy_dir.mkdir()
        dead = subprocess.Popen(["true"])
        dead.wait()
        (heavy_dir / f"odin3-heavy.queued.{dead.pid}").write_text(f"{dead.pid}\n")
        live = subprocess.Popen(["sleep", "30"])
        try:
            (heavy_dir / f"odin3-heavy.queued.{live.pid}").write_text(f"{live.pid}\n")
            build = subprocess.Popen(
                [BASH, str(BUILD_LOCKED), "--preset", "debug"], text=True,
                env={**self.env, "ODIN3_HEAVY_DIR": str(heavy_dir), "ODIN3_HEAVY_POLL": "0.1"},
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(1.0)
            self.assertIsNone(build.poll())  # still waiting for the live heavy run
            self.assertEqual(self.calls(), [])
            live.send_signal(signal.SIGTERM)
            live.wait()
            _, err = build.communicate(timeout=30)
        finally:
            live.kill()
            live.wait()
        self.assertEqual(build.returncode, 0)
        self.assertEqual(err, f"build-locked: waiting for queued heavy run {live.pid}\n")
        self.assertEqual(self.calls(), [
            f"flock -s {heavy_dir}/odin3-heavy.lock cmake --build --preset debug"
            " | flag=no fd9=none"])

    def test_default_lock(self) -> None:
        if any(DEFAULT_DIR.glob("odin3-heavy.queued.*")):
            self.skipTest("a heavy run is queued on this machine")
        try:
            run = subprocess.run([BASH, str(BUILD_LOCKED), "--preset", "debug", "-j4"],
                                 env=self.env, capture_output=True, text=True, timeout=30,
                                 check=False)
        except subprocess.TimeoutExpired:
            self.skipTest("a heavy run was queued meanwhile")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(self.calls(), [
            f"flock -s {DEFAULT_DIR}/odin3-heavy.lock cmake --build --preset debug -j4"
            " | flag=no fd9=none"])


class SweepTestFunctionTest(unittest.TestCase):
    def test_configure_time_checks(self) -> None:
        cmake = os.environ.get("ODIN3_CMAKE") or shutil.which("cmake")
        if cmake is None:
            self.skipTest("no cmake")
        with tempfile.TemporaryDirectory() as work:
            run = subprocess.run(
                [cmake, f"-DODIN3_SOURCE={helpers.REPO_ROOT}", f"-DWORK={work}", "-P",
                 str(helpers.REPO_ROOT / "tests" / "tools" / "sweep_test_check.cmake")],
                capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("odin3_add_sweep_test: ok", run.stdout)


def _user_manager() -> bool:
    if shutil.which("systemd-run") is None or shutil.which("systemctl") is None:
        return False
    probe = subprocess.run(["systemctl", "--user", "show-environment"], capture_output=True,
                           timeout=30, check=False)
    return probe.returncode == 0


class LiveProbeTest(unittest.TestCase):
    def test_slice_cap_is_set(self) -> None:
        if not _user_manager():
            self.skipTest("no systemd-run --user")
        env = {k: v for k, v in os.environ.items() if not k.startswith("ODIN3_HEAVY")}
        run = subprocess.run([BASH, str(HEAVY_RUN), "true"],
                             env={**env, "ODIN3_HEAVY_OK": "1", "ODIN3_HEAVY_WAIT": "10"},
                             capture_output=True, text=True, timeout=120, check=False)
        if run.returncode == SKIP:
            self.skipTest(run.stderr.strip())
        self.assertEqual(run.returncode, 0, run.stderr)
        show = subprocess.run(["systemctl", "--user", "show", "odin3-heavy.slice", "-p",
                               "MemoryMax"], capture_output=True, text=True, timeout=30,
                              check=False)
        self.assertEqual(show.stdout.strip(), "MemoryMax=6442450944")


if __name__ == "__main__":
    unittest.main()

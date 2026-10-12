"""Tests for tools/heavy-run, tools/build-locked and odin3_add_sweep_test (2E plan point (p), ruling
C1).

Every test runs under a private ODIN3_HEAVY_DIR, so the gate never touches the machine's heavy
lock or its queued flags. The argv tests use fake flock, systemctl and systemd-run scripts on PATH
that log their argv; the lock tests use the real flock(1) with the fake systemd-run (which runs
the job in place) to show that the lock is held for the job's whole life. The live probe at the
end runs the real heavy-run (`true`) and reads the slice's cap back; it is skipped without a
systemd --user manager.
"""

from __future__ import annotations

import fcntl
import os
import re
import shutil
import signal
import stat
import subprocess
import tempfile
import time
import unittest
from collections.abc import Callable
from pathlib import Path

from . import helpers

HEAVY_RUN = helpers.REPO_ROOT / "tools" / "heavy-run"
BUILD_LOCKED = helpers.REPO_ROOT / "tools" / "build-locked"
BASH = shutil.which("bash") or "/bin/bash"
SKIP = 77
WAIT_S = 10.0

# A cgroup line as /proc/self/cgroup reads outside any heavy slice.
CGROUP_OUT = "0::/user.slice/user-1000.slice/user@1000.service/app.slice/run-r0.scope\n"

# Each fake appends one line per call to $FAKE_LOG: its name and argv, then key=value facts.
FAKE_SYSTEMCTL = """#!{bash}
echo "systemctl $*" >>"$FAKE_LOG"
exit "${{FAKE_SYSTEMCTL_RC:-0}}"
"""
# flock: heavy-run calls `flock -x -w N 9` on its lock descriptor; build-locked probes with
# `flock -s -n LOCK true`, then execs `flock -s LOCK cmake --build ...` (logged, not run).
FAKE_FLOCK = """#!{bash}
flag=no
[[ -e $ODIN3_HEAVY_DIR/odin3-heavy.queued.$PPID ]] && flag=yes
fd9=$(readlink /proc/$$/fd/9 2>/dev/null || echo none)
echo "flock $* | flag=$flag fd9=$fd9" >>"$FAKE_LOG"
exit "${{FAKE_FLOCK_RC:-0}}"
"""
# systemd-run runs the job in place, as the real `--scope` does (the job keeps fd 9).
FAKE_SYSTEMD_RUN = """#!{bash}
flag=no
[[ -n $(ls "$ODIN3_HEAVY_DIR"/odin3-heavy.queued.$PPID 2>/dev/null) ]] && flag=yes
echo "systemd-run $* | slice=${{ODIN3_HEAVY_SLICE:-}} flag=$flag" >>"$FAKE_LOG"
while [[ $1 == -* ]]; do shift; done
exec "$@"
"""
FAKE_CMAKE = """#!{bash}
echo "cmake $*" >>"$FAKE_LOG"
"""


def _write_exec(path: Path, text: str) -> None:
    path.write_text(text.format(bash=BASH))
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def _lock_free(lock: Path) -> bool:
    """True when nobody holds lock: a non-blocking exclusive flock(1) succeeds."""
    run = subprocess.run(["flock", "-n", "-x", str(lock), "true"], timeout=30, check=False)
    return run.returncode == 0


def _wait_for(cond: Callable[[], bool], what: str) -> None:
    deadline = time.monotonic() + WAIT_S
    while time.monotonic() < deadline:
        if cond():
            return
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


class FakeTools(unittest.TestCase):
    """A temporary bin directory of fakes first on PATH, their log, and a private heavy dir."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.bin = self.dir / "bin"
        self.bin.mkdir()
        _write_exec(self.bin / "systemctl", FAKE_SYSTEMCTL)
        _write_exec(self.bin / "flock", FAKE_FLOCK)
        _write_exec(self.bin / "systemd-run", FAKE_SYSTEMD_RUN)
        _write_exec(self.bin / "cmake", FAKE_CMAKE)
        self.log = self.dir / "fake.log"
        self.heavy = self.dir / "heavy"
        self.lock = self.heavy / "odin3-heavy.lock"
        self.env = {
            k: v for k, v in os.environ.items() if not k.startswith(("ODIN3_HEAVY", "FAKE_"))
        }
        self.env["PATH"] = f"{self.bin}:{os.environ.get('PATH', '/usr/bin:/bin')}"
        self.env["FAKE_LOG"] = str(self.log)
        self.env["ODIN3_HEAVY_DIR"] = str(self.heavy)
        # outside any heavy slice by default, even when ctest itself runs under heavy-run
        outside = self.dir / "cgroup-outside"
        outside.write_text(CGROUP_OUT)
        self.env["ODIN3_HEAVY_CGROUP"] = str(outside)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def real_flock(self) -> None:
        """Use the real flock(1) instead of the fake."""
        (self.bin / "flock").unlink()

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
        calls = self.calls()
        self.assertEqual(len(calls), 4, calls)
        self.assertEqual(calls[0], "systemctl --user set-property --runtime odin3-heavy.slice "
                                   "MemoryMax=6G MemorySwapMax=0")
        # the flag is written before the wait, which is on the lock's descriptor ...
        self.assertEqual(calls[1], f"flock -x -w 14400 9 | flag=yes fd9={self.lock}")
        # ... and removed once the lock is held; the job runs in a named scope of the slice
        unit = re.fullmatch(r"systemd-run --user --scope -q "
                            r"--unit=(odin3-heavy-[0-9]+-[0-9]+) "
                            r"--slice=odin3-heavy\.slice echo hi \| slice=odin3-heavy\.slice "
                            r"flag=no", calls[2])
        self.assertIsNotNone(unit, calls[2])
        assert unit is not None
        self.assertEqual(calls[3], f"systemctl --user stop {unit.group(1)}.scope")
        self.assertEqual(sorted(p.name for p in self.heavy.iterdir()), ["odin3-heavy.lock"])

    def test_the_slice_is_stopped_after_the_job(self) -> None:
        """Strays outside the job's scope (memcap.py's own scopes) go with the slice; under a
        private ODIN3_HEAVY_DIR only with ODIN3_HEAVY_STOP_SLICE=1 (review finding 3)."""
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", ODIN3_HEAVY_STOP_SLICE="1")
        self.assertEqual(run.returncode, 0, run.stderr)
        calls = self.calls()
        self.assertEqual(len(calls), 5, calls)
        self.assertRegex(calls[3], r"^systemctl --user stop odin3-heavy-[0-9]+-[0-9]+\.scope$")
        self.assertEqual(calls[4], "systemctl --user stop odin3-heavy.slice")
        self.assertIn('[[ $dir == /tmp/claude-1000 || ${ODIN3_HEAVY_STOP_SLICE:-} == 1 ]]',
                      HEAVY_RUN.read_text())  # the machine's lock always stops it

    def test_the_flag_names_the_pid(self) -> None:
        script = self.dir / "show-flag"
        _write_exec(script, '#!{bash}\nls "$ODIN3_HEAVY_DIR" >>"$FAKE_LOG"\n')
        _write_exec(self.bin / "flock", '#!{bash}\nls "$ODIN3_HEAVY_DIR" >>"$FAKE_LOG"\n'
                    'cat "$ODIN3_HEAVY_DIR"/odin3-heavy.queued.* >>"$FAKE_LOG"\n')
        run = self.run_tool(HEAVY_RUN, str(script), ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, 0, run.stderr)
        calls = self.calls()
        flag = next(line for line in calls if line.startswith("odin3-heavy.queued."))
        self.assertEqual(calls[calls.index(flag) + 1], flag.rsplit(".", 1)[1])  # its PID inside
        self.assertEqual(calls[-2], "odin3-heavy.lock")  # the job runs without the flag
        self.assertEqual(sorted(p.name for p in self.heavy.iterdir()), ["odin3-heavy.lock"])

    def test_lock_timeout_skips(self) -> None:
        run = self.run_tool(HEAVY_RUN, "echo", "hi", ODIN3_HEAVY_OK="1", FAKE_FLOCK_RC="1")
        self.assertEqual(run.returncode, SKIP)
        self.assertEqual(run.stderr, "heavy-run: lock busy for 14400 s; skipped\n")
        self.assertFalse(any(line.startswith("systemd-run") for line in self.calls()))
        self.assertEqual([p.name for p in self.heavy.iterdir()], ["odin3-heavy.lock"])

    def test_wait_is_odin3_heavy_wait(self) -> None:
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", ODIN3_HEAVY_WAIT="5",
                            FAKE_FLOCK_RC="1")
        self.assertEqual(run.returncode, SKIP)
        self.assertIn("lock busy for 5 s", run.stderr)
        self.assertTrue(self.calls()[1].startswith("flock -x -w 5 9 "))

    def test_job_status_is_returned(self) -> None:
        run = self.run_tool(HEAVY_RUN, "sh", "-c", "exit 1", ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, 1)  # a job's exit 1 is not a busy lock

    def test_xl_slice(self) -> None:
        run = self.run_tool(HEAVY_RUN, "--xl", "true", ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, SKIP)
        self.assertIn("ODIN3_HEAVY_XL_OK", run.stderr)
        self.assertEqual(self.calls(), [])
        run = self.run_tool(HEAVY_RUN, "--xl", "true", ODIN3_HEAVY_OK="1", ODIN3_HEAVY_XL_OK="1")
        self.assertEqual(run.returncode, 0, run.stderr)
        calls = self.calls()
        self.assertEqual(calls[0], "systemctl --user set-property --runtime odin3-heavyxl.slice "
                                   "MemoryMax=9G MemorySwapMax=0")
        self.assertRegex(calls[2], r"^systemd-run --user --scope -q "
                                   r"--unit=odin3-heavy-[0-9]+-[0-9]+ "
                                   r"--slice=odin3-heavyxl\.slice true \| slice=odin3-heavyxl")

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
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", FAKE_SYSTEMCTL_RC="1")
        self.assertEqual(run.returncode, SKIP)
        self.assertIn("cannot set up odin3-heavy.slice", run.stderr)

    def test_usage(self) -> None:
        run = self.run_tool(HEAVY_RUN, ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, 2)

    def test_default_dir(self) -> None:
        for tool in (HEAVY_RUN, BUILD_LOCKED):
            self.assertIn("dir=${ODIN3_HEAVY_DIR:-/tmp/claude-1000}", tool.read_text())


# A cgroup line as /proc/self/cgroup reads inside a scope of the slice.
CGROUP_IN = "0::/user.slice/user-1000.slice/user@1000.service/odin3.slice/{slice}/run-r1.scope\n"


class NestingTest(FakeTools):
    """Inside a heavy run heavy-run runs the job as is and builds refuse; the evidence is the
    process's cgroup (faked with ODIN3_HEAVY_CGROUP) and the heavy lock on fd 9, never the variable
    ODIN3_HEAVY_SLICE. In the slice without fd 9 both exit 2 at once."""

    def cgroup_file(self, slice_name: str) -> str:
        path = self.dir / f"cgroup-{slice_name}"
        path.write_text(CGROUP_IN.format(slice=slice_name))
        return str(path)

    def inside(self, tool: Path, *args: str, slice_name: str = "odin3-heavy.slice",
               fd9: bool = True, in_slice: bool = True, env_slice: str | None = None,
               **extra: str) -> subprocess.CompletedProcess[str]:
        """Runs tool as a heavy run's job would: in the slice (unless not in_slice), the heavy
        lock open on fd 9 (unless not fd9), and ODIN3_HEAVY_SLICE=env_slice (default
        slice_name; empty unsets it)."""
        self.heavy.mkdir(exist_ok=True)
        opener = f'exec 9>>"{self.lock}"; ' if fd9 else ""
        env = {**self.env, **extra}
        env_slice = slice_name if env_slice is None else env_slice
        if env_slice:
            env["ODIN3_HEAVY_SLICE"] = env_slice
        if in_slice:
            env["ODIN3_HEAVY_CGROUP"] = self.cgroup_file(slice_name)
        script = f'{opener}exec {BASH} "$@"'
        return subprocess.run([BASH, "-c", script, "job", str(tool), *args], env=env,
                              capture_output=True, text=True, timeout=60, check=False)

    def test_nested_heavy_run_runs_in_place(self) -> None:
        run = self.inside(HEAVY_RUN, "echo", "inner")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "inner\n")
        self.assertEqual(run.stderr, "")
        self.assertEqual(self.calls(), [])  # no slice set-up, no lock, no scope

    def test_nested_xl(self) -> None:
        run = self.inside(HEAVY_RUN, "--xl", "true")
        self.assertEqual(run.returncode, 2)
        self.assertEqual(run.stderr, "heavy-run: --xl inside odin3-heavy.slice (6G): run the xl "
                                     "job on its own\n")
        run = self.inside(HEAVY_RUN, "--xl", "echo", "xl", slice_name="odin3-heavyxl.slice")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "xl\n")

    def test_build_locked_refused_inside(self) -> None:
        run = self.inside(BUILD_LOCKED, "--preset", "debug")
        self.assertEqual(run.returncode, 2)
        self.assertIn("build-locked: refused inside a heavy run (odin3-heavy.slice)", run.stderr)
        self.assertEqual(self.calls(), [])

    def test_in_the_slice_without_the_descriptor_fails_at_once(self) -> None:
        """A parent closed fd 9 (Python's close_fds): exit 2, no wait, no lock (final review I1)."""
        self.real_flock()
        self.heavy.mkdir()
        holder = os.open(self.lock, os.O_WRONLY | os.O_CREAT | os.O_APPEND)
        try:
            fcntl.flock(holder, fcntl.LOCK_EX)  # the ancestor heavy run's lock
            for env_slice in ("odin3-heavy.slice", ""):  # the variable does not matter
                start = time.monotonic()
                run = self.inside(HEAVY_RUN, "echo", "never", fd9=False, env_slice=env_slice,
                                  ODIN3_HEAVY_OK="1", ODIN3_HEAVY_WAIT="30")
                self.assertLess(time.monotonic() - start, 10.0)
                self.assertEqual(run.returncode, 2)
                self.assertEqual(run.stdout, "")
                self.assertEqual(run.stderr, "heavy-run: nested heavy-run lost the lock "
                                             "descriptor; the parent must pass fd 9 (Python: "
                                             "pass_fds=(9,))\n")
                start = time.monotonic()
                run = self.inside(BUILD_LOCKED, "--preset", "debug", fd9=False,
                                  env_slice=env_slice)
                self.assertLess(time.monotonic() - start, 10.0)
                self.assertEqual(run.returncode, 2)
                self.assertIn("build-locked: refused inside a heavy run (odin3-heavy.slice)",
                              run.stderr)
        finally:
            os.close(holder)
        self.assertEqual(self.calls(), [])  # no slice set-up, no scope, no build
        self.assertEqual([p.name for p in self.heavy.iterdir()], ["odin3-heavy.lock"])

    def test_under_build_locked_fails_at_once(self) -> None:
        """The heavy lock open outside a heavy slice (build-locked's shared lock, inherited by
        every build child): exit 2 instead of waiting on its own tree (review finding 2)."""
        run = self.inside(HEAVY_RUN, "echo", "never", in_slice=False, ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, 2)
        self.assertEqual(run.stdout, "")
        self.assertIn("heavy-run: started under build-locked", run.stderr)
        self.assertIn("run the sweep outside the build", run.stderr)
        self.assertEqual(self.calls(), [])

    def test_stale_variable_is_ignored(self) -> None:
        """ODIN3_HEAVY_SLICE outside a heavy slice: warned, unset, the normal path (review r2)."""
        warning = ("heavy-run: warning: ODIN3_HEAVY_SLICE=odin3-heavy.slice is set outside a "
                   "heavy run (not in that slice); ignoring it\n")
        run = self.run_tool(HEAVY_RUN, "echo", "never", ODIN3_HEAVY_SLICE="odin3-heavy.slice")
        self.assertEqual(run.returncode, SKIP)  # no ODIN3_HEAVY_OK: skipped, not run uncapped
        self.assertEqual(run.stdout, "")
        self.assertEqual(run.stderr, warning + "heavy-run: ODIN3_HEAVY_OK is not 1; skipped\n")
        run = self.run_tool(HEAVY_RUN, "echo", "hi", ODIN3_HEAVY_OK="1",
                            ODIN3_HEAVY_SLICE="odin3-heavy.slice")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "hi\n")
        calls = self.calls()
        self.assertTrue(calls[1].startswith("flock -x -w 14400 9 "), calls)  # the lock is taken
        self.assertIn("--slice=odin3-heavy.slice echo hi", calls[2])

    def test_stale_variable_does_not_stop_a_build(self) -> None:
        run = self.run_tool(BUILD_LOCKED, "--preset", "debug",
                            ODIN3_HEAVY_SLICE="odin3-heavy.slice")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("build-locked: warning: ODIN3_HEAVY_SLICE=odin3-heavy.slice is set outside",
                      run.stderr)
        self.assertIn("(not in that slice); ignoring it", run.stderr)
        self.assertEqual(self.calls()[-1],
                         f"flock -s {self.lock} cmake --build --preset debug | flag=no fd9=none")

    def test_nested_with_the_real_lock(self) -> None:
        """A heavy run whose job calls heavy-run and build-locked: no wait on its own lock."""
        self.real_flock()
        # the job enters the slice (the fake systemd-run cannot move it there)
        inner = (f"export ODIN3_HEAVY_CGROUP={self.cgroup_file('odin3-heavy.slice')}; "
                 f"{BASH} {HEAVY_RUN} echo inner && "
                 f"{{ {BASH} {BUILD_LOCKED} --preset debug; echo build=$?; }}")
        start = time.monotonic()
        run = self.run_tool(HEAVY_RUN, BASH, "-c", inner, ODIN3_HEAVY_OK="1",
                            ODIN3_HEAVY_WAIT="5")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "inner\nbuild=2\n")
        self.assertLess(time.monotonic() - start, 5.0)

    def test_python_driver_must_pass_fd9(self) -> None:
        """A Python driver's nested step runs in place with pass_fds=(9,), and fails at once
        (exit 2, not a 4 h wait) without it, since subprocess closes fd 9 by default."""
        self.real_flock()
        driver = ("import subprocess, sys\n"
                  "for fds in ((9,), ()):\n"
                  f"    step = subprocess.run([{BASH!r}, {str(HEAVY_RUN)!r}, 'echo', 'step'],"
                  " pass_fds=fds)\n"
                  "    print(f'rc={step.returncode}', flush=True)\n")
        inner = (f"export ODIN3_HEAVY_CGROUP={self.cgroup_file('odin3-heavy.slice')}; "
                 f"exec python3 -I -c \"$1\"")
        start = time.monotonic()
        run = self.run_tool(HEAVY_RUN, BASH, "-c", inner, "job", driver, ODIN3_HEAVY_OK="1",
                            ODIN3_HEAVY_WAIT="5")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "step\nrc=0\nrc=2\n")
        self.assertIn("pass_fds=(9,)", run.stderr)
        self.assertLess(time.monotonic() - start, 5.0)


class RealLockTest(FakeTools):
    """The real flock(1): the job holds the lock for its whole life, the wrapper's fate aside."""

    def setUp(self) -> None:
        super().setUp()
        self.real_flock()

    def test_lock_is_held_by_the_job_and_released_after(self) -> None:
        probe = f"flock -n -x {self.lock} true && echo free || echo held"
        run = self.run_tool(HEAVY_RUN, BASH, "-c", probe, ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "held\n")
        self.assertTrue(_lock_free(self.lock))

    def test_lock_survives_a_sigkilled_wrapper(self) -> None:
        pid_file = self.dir / "job.pid"
        go = self.dir / "go"
        job = f"echo $$ >{pid_file}; while [ ! -e {go} ]; do sleep 0.05; done"
        wrapper = subprocess.Popen([BASH, str(HEAVY_RUN), BASH, "-c", job],
                                   env={**self.env, "ODIN3_HEAVY_OK": "1"},
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            _wait_for(pid_file.exists, "the job to start")
            self.assertFalse(_lock_free(self.lock))
            wrapper.send_signal(signal.SIGKILL)
            wrapper.wait(timeout=30)
            self.assertFalse(_lock_free(self.lock))  # the running job still holds it
        finally:
            go.touch()
            wrapper.kill()
            wrapper.wait(timeout=30)
        _wait_for(lambda: _lock_free(self.lock), "the lock to be released")

    def test_busy_lock_skips(self) -> None:
        self.heavy.mkdir()
        holder = os.open(self.lock, os.O_WRONLY | os.O_CREAT | os.O_APPEND)
        try:
            fcntl.flock(holder, fcntl.LOCK_EX)
            run = self.run_tool(HEAVY_RUN, "echo", "never", ODIN3_HEAVY_OK="1",
                                ODIN3_HEAVY_WAIT="1")
        finally:
            os.close(holder)
        self.assertEqual(run.returncode, SKIP)
        self.assertEqual(run.stdout, "")
        self.assertEqual(run.stderr, "heavy-run: lock busy for 1 s; skipped\n")
        self.assertEqual([p.name for p in self.heavy.iterdir()], ["odin3-heavy.lock"])

    def test_build_waits_for_the_holder(self) -> None:
        self.heavy.mkdir()
        holder = os.open(self.lock, os.O_WRONLY | os.O_CREAT | os.O_APPEND)
        try:
            fcntl.flock(holder, fcntl.LOCK_EX)
            build = subprocess.Popen([BASH, str(BUILD_LOCKED), "--preset", "debug"], text=True,
                                     env=self.env, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
            time.sleep(1.0)
            self.assertIsNone(build.poll())
            self.assertEqual(self.calls(), [])
        finally:
            os.close(holder)
        _, err = build.communicate(timeout=30)
        self.assertEqual(build.returncode, 0)
        self.assertEqual(err, f"build-locked: waiting for the heavy run that holds {self.lock}\n")
        self.assertEqual(self.calls(), ["cmake --build --preset debug"])


class BuildLockedTest(FakeTools):
    def test_waits_for_a_live_queued_heavy_run(self) -> None:
        self.heavy.mkdir()
        dead = subprocess.Popen(["true"])
        dead.wait()
        dead_flag = self.heavy / f"odin3-heavy.queued.{dead.pid}"
        dead_flag.write_text(f"{dead.pid}\n")
        # a live PID that is not a heavy-run (a reused PID): stale too
        other = subprocess.Popen(["sleep", "30"], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        other_flag = self.heavy / f"odin3-heavy.queued.{other.pid}"
        other_flag.write_text(f"{other.pid}\n")
        live = subprocess.Popen([BASH, "-c", "exec -a heavy-run sleep 30"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            (self.heavy / f"odin3-heavy.queued.{live.pid}").write_text(f"{live.pid}\n")
            build = subprocess.Popen(
                [BASH, str(BUILD_LOCKED), "--preset", "debug"], text=True,
                env={**self.env, "ODIN3_HEAVY_POLL": "0.1"},
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            time.sleep(1.0)
            self.assertIsNone(build.poll())  # still waiting for the live heavy run
            self.assertEqual(self.calls(), [])
            self.assertFalse(dead_flag.exists())
            self.assertFalse(other_flag.exists())
            live.send_signal(signal.SIGTERM)
            live.wait()
            _, err = build.communicate(timeout=30)
        finally:
            for proc in (live, other):
                proc.kill()
                proc.wait()
        self.assertEqual(build.returncode, 0)
        self.assertEqual(err, f"build-locked: waiting for queued heavy run {live.pid}\n")
        self.assertEqual(self.calls(), [
            f"flock -s -n {self.lock} true | flag=no fd9=none",
            f"flock -s {self.lock} cmake --build --preset debug | flag=no fd9=none"])


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
        if "/odin3-heavy" in Path("/proc/self/cgroup").read_text():
            self.skipTest("already inside a heavy slice")
        env = {k: v for k, v in os.environ.items() if not k.startswith("ODIN3_HEAVY")}
        with tempfile.TemporaryDirectory() as heavy:  # never the machine's lock
            run = subprocess.run([BASH, str(HEAVY_RUN), "true"],
                                 env={**env, "ODIN3_HEAVY_OK": "1", "ODIN3_HEAVY_DIR": heavy},
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

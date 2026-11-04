"""Tests for tools/heavy-run (two slots), tools/build-locked and odin3_add_sweep_test (2E plan
point (p), ruling C1; two slots 2026-11-04).

Every test runs under a private ODIN3_HEAVY_DIR and a private slice prefix
(ODIN3_HEAVY_SLICE_PREFIX=odin3-htest), so the gate never touches the machine's heavy locks, queued
flags or slices. The argv tests use fake flock, systemctl and systemd-run scripts on PATH
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

PREFIX = "odin3-htest"
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
# flock: heavy-run calls `flock -s -w N 9` (legacy lock), then `flock -x -n 8` / `-x -n 7` (slots)
# or
# `-x -w N 8` / `-x -w N 7` on the slot descriptors; build-locked probes with `flock -s -n FILE
# true` per lock file, then `flock -s 9`, `-s 8`, `-s 7`. FAKE_FLOCK_RC is the status, only for
# calls whose argv contains FAKE_FLOCK_ONLY when that is set.
FAKE_FLOCK = """#!{bash}
flag=no
[[ -e $ODIN3_HEAVY_DIR/odin3-heavy.queued.$PPID ]] && flag=yes
fd9=$(readlink /proc/$$/fd/9 2>/dev/null || echo none)
echo "flock $* | flag=$flag fd9=$fd9" >>"$FAKE_LOG"
rc=${{FAKE_FLOCK_RC:-0}}
if [[ -n ${{FAKE_FLOCK_ONLY:-}} && "$*" != *"$FAKE_FLOCK_ONLY"* ]]; then rc=0; fi
exit "$rc"
"""
# systemd-run runs the job in place, as the real `--scope` does (the job keeps fd 9).
FAKE_SYSTEMD_RUN = """#!{bash}
flag=no
[[ -n $(ls "$ODIN3_HEAVY_DIR"/odin3-heavy.queued.$PPID 2>/dev/null) ]] && flag=yes
echo "systemd-run $* | slice=${{ODIN3_HEAVY_SLICE:-}} flag=$flag" >>"$FAKE_LOG"
while [[ $1 == -* ]]; do [[ $1 == -p ]] && shift; shift; done
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
        self.slot0 = self.heavy / "odin3-heavy.lock.0"
        self.slot1 = self.heavy / "odin3-heavy.lock.1"
        self.env = {
            k: v for k, v in os.environ.items() if not k.startswith(("ODIN3_HEAVY", "FAKE_"))
        }
        self.env["PATH"] = f"{self.bin}:{os.environ.get('PATH', '/usr/bin:/bin')}"
        self.env["FAKE_LOG"] = str(self.log)
        self.env["ODIN3_HEAVY_DIR"] = str(self.heavy)
        self.env["ODIN3_HEAVY_SLICE_PREFIX"] = PREFIX
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
        self.assertEqual(run.stderr, f"heavy-run: slot 0 ({PREFIX}0.slice)\n")
        calls = self.calls()
        self.assertEqual(len(calls), 5, calls)
        # the flag is written before the waits; the legacy lock is taken shared on fd 9 ...
        self.assertRegex(calls[0], rf"^flock -s -w [0-9]+ 9 \| flag=yes fd9={self.lock}$")
        # ... then the first free slot, exclusive, without waiting
        self.assertEqual(calls[1], f"flock -x -n 8 | flag=yes fd9={self.lock}")
        self.assertEqual(calls[2], f"systemctl --user set-property --runtime {PREFIX}0.slice "
                                   "MemoryMax=6G MemorySwapMax=0")
        # the flag is gone once the slot is held; the job runs in a named scope of the slice
        unit = re.fullmatch(rf"systemd-run --user --scope -q "
                            rf"--unit=({PREFIX}-[0-9]+-[0-9]+) "
                            rf"--slice={PREFIX}0\.slice -p OOMPolicy=continue echo hi "
                            rf"\| slice={PREFIX}0\.slice flag=no", calls[3])
        self.assertIsNotNone(unit, calls[3])
        assert unit is not None
        self.assertEqual(calls[4], f"systemctl --user stop {unit.group(1)}.scope")
        self.assertEqual(sorted(p.name for p in self.heavy.iterdir()),
                         ["odin3-heavy.lock", "odin3-heavy.lock.0", "odin3-heavy.lock.1"])

    def test_slot_one_when_slot_zero_is_busy(self) -> None:
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", FAKE_FLOCK_RC="1",
                            FAKE_FLOCK_ONLY="-x -n 8")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stderr, f"heavy-run: slot 1 ({PREFIX}1.slice)\n")
        calls = self.calls()
        self.assertEqual(calls[2][:12], "flock -x -n ")
        self.assertIn(f"--slice={PREFIX}1.slice", calls[4])

    def test_both_busy_waits_on_both_slots(self) -> None:
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", ODIN3_HEAVY_WAIT="50",
                            FAKE_FLOCK_RC="1", FAKE_FLOCK_ONLY="-x")
        self.assertEqual(run.returncode, SKIP)
        self.assertRegex(run.stderr, r"^heavy-run: lock busy for 50 s; skipped\n$")
        waits = sorted(line.split(" |")[0][:-2] for line in self.calls() if "-x -w" in line)
        self.assertEqual(len(waits), 2, self.calls())
        self.assertTrue(all(re.fullmatch(r"flock -x -w (49|50)", w) for w in waits), waits)
        self.assertFalse(any(line.startswith("systemd-run") for line in self.calls()))

    def test_the_slice_is_stopped_after_the_job(self) -> None:
        """Strays outside the job's scope (memcap.py's own scopes) go with the slice; under a
        private ODIN3_HEAVY_DIR only with ODIN3_HEAVY_STOP_SLICE=1 (review finding 3)."""
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", ODIN3_HEAVY_STOP_SLICE="1")
        self.assertEqual(run.returncode, 0, run.stderr)
        calls = self.calls()
        self.assertEqual(len(calls), 6, calls)
        self.assertRegex(calls[4], rf"^systemctl --user stop {PREFIX}-[0-9]+-[0-9]+\.scope$")
        self.assertEqual(calls[5], f"systemctl --user stop {PREFIX}0.slice")
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
        self.assertEqual(calls[-4:-1], ["odin3-heavy.lock", "odin3-heavy.lock.0",
                                      "odin3-heavy.lock.1"])  # the job runs without the flag
        self.assertEqual(sorted(p.name for p in self.heavy.iterdir()),
                         ["odin3-heavy.lock", "odin3-heavy.lock.0", "odin3-heavy.lock.1"])

    def test_lock_timeout_skips(self) -> None:
        run = self.run_tool(HEAVY_RUN, "echo", "hi", ODIN3_HEAVY_OK="1", FAKE_FLOCK_RC="1")
        self.assertEqual(run.returncode, SKIP)
        self.assertEqual(run.stderr, "heavy-run: lock busy for 14400 s; skipped\n")
        self.assertFalse(any(line.startswith("systemd-run") for line in self.calls()))

    def test_wait_is_odin3_heavy_wait(self) -> None:
        run = self.run_tool(HEAVY_RUN, "true", ODIN3_HEAVY_OK="1", ODIN3_HEAVY_WAIT="5",
                            FAKE_FLOCK_RC="1")
        self.assertEqual(run.returncode, SKIP)
        self.assertIn("lock busy for 5 s", run.stderr)
        self.assertRegex(self.calls()[0], r"^flock -s -w [45] 9 ")

    def test_job_status_is_returned(self) -> None:
        run = self.run_tool(HEAVY_RUN, "sh", "-c", "exit 1", ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, 1)  # a job's exit 1 is not a busy lock

    def test_xl_and_solo_take_both_slots(self) -> None:
        run = self.run_tool(HEAVY_RUN, "--xl", "true", ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, SKIP)
        self.assertIn("ODIN3_HEAVY_XL_OK", run.stderr)
        self.assertEqual(self.calls(), [])
        for flag, name, cap in (("--xl", "xl", "9G"), ("--solo", "solo", "6G")):
            self.log.unlink(missing_ok=True)
            run = self.run_tool(HEAVY_RUN, flag, "true", ODIN3_HEAVY_OK="1",
                                ODIN3_HEAVY_XL_OK="1")
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(run.stderr, f"heavy-run: slot {name} ({PREFIX}{name}.slice)\n")
            calls = self.calls()
            self.assertRegex(calls[1], rf"^flock -x -w [0-9]+ 8 \| flag=yes fd9={self.lock}$")
            self.assertRegex(calls[2], rf"^flock -x -w [0-9]+ 7 \| flag=yes fd9={self.lock}$")
            self.assertEqual(calls[3], f"systemctl --user set-property --runtime "
                                       f"{PREFIX}{name}.slice MemoryMax={cap} MemorySwapMax=0")
            self.assertRegex(calls[4], rf"^systemd-run --user --scope -q "
                                       rf"--unit={PREFIX}-[0-9]+-[0-9]+ "
                                       rf"--slice={PREFIX}{name}\.slice -p OOMPolicy=continue true "
                                       rf"\| slice={PREFIX}{name}\.slice")

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
        self.assertIn(f"cannot set up {PREFIX}0.slice", run.stderr)

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

    def inside(self, tool: Path, *args: str, slice_name: str = f"{PREFIX}0.slice",
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
        self.assertEqual(run.stderr, f"heavy-run: --xl inside {PREFIX}0.slice (6G): run the xl "
                                     "job on its own\n")
        run = self.inside(HEAVY_RUN, "--xl", "echo", "xl", slice_name=f"{PREFIX}xl.slice")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "xl\n")

    def test_nested_solo(self) -> None:
        """--solo needs a slice that holds the whole machine (solo, xl, the old single slice)."""
        for name in (f"{PREFIX}0.slice", f"{PREFIX}1.slice"):
            run = self.inside(HEAVY_RUN, "--solo", "true", slice_name=name)
            self.assertEqual(run.returncode, 2)
            self.assertEqual(run.stderr, f"heavy-run: --solo inside {name} (one slot): run the "
                                         "solo job on its own\n")
        for name in (f"{PREFIX}solo.slice", f"{PREFIX}xl.slice", "odin3-heavy.slice"):
            run = self.inside(HEAVY_RUN, "--solo", "echo", "solo", slice_name=name)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertEqual(run.stdout, "solo\n")
        self.assertEqual(self.calls(), [])

    def test_build_locked_refused_inside(self) -> None:
        run = self.inside(BUILD_LOCKED, "--preset", "debug")
        self.assertEqual(run.returncode, 2)
        self.assertIn(f"build-locked: refused inside a heavy run ({PREFIX}0.slice)", run.stderr)
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
                                  ODIN3_HEAVY_OK="1", ODIN3_HEAVY_WAIT="60")
                self.assertLess(time.monotonic() - start, 30.0)  # a lock wait would last 60 s
                self.assertEqual(run.returncode, 2)
                self.assertEqual(run.stdout, "")
                self.assertEqual(run.stderr, "heavy-run: nested heavy-run lost the lock "
                                             "descriptor; the parent must pass fd 9 (Python: "
                                             "pass_fds=(9,))\n")
                start = time.monotonic()
                run = self.inside(BUILD_LOCKED, "--preset", "debug", fd9=False,
                                  env_slice=env_slice)
                self.assertLess(time.monotonic() - start, 30.0)
                self.assertEqual(run.returncode, 2)
                self.assertIn(f"build-locked: refused inside a heavy run ({PREFIX}0.slice)",
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
        self.assertTrue(calls[0].startswith("flock -s -w "), calls)  # the lock is taken
        self.assertIn(f"--slice={PREFIX}0.slice -p OOMPolicy=continue echo hi", calls[3])

    def test_stale_variable_does_not_stop_a_build(self) -> None:
        run = self.run_tool(BUILD_LOCKED, "--preset", "debug",
                            ODIN3_HEAVY_SLICE="odin3-heavy.slice")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("build-locked: warning: ODIN3_HEAVY_SLICE=odin3-heavy.slice is set outside",
                      run.stderr)
        self.assertIn("(not in that slice); ignoring it", run.stderr)
        self.assertEqual(self.calls()[-1], "cmake --build --preset debug")

    def test_nested_with_the_real_lock(self) -> None:
        """A heavy run whose job calls heavy-run and build-locked: no wait on its own lock."""
        self.real_flock()
        # the job enters the slice (the fake systemd-run cannot move it there)
        inner = (f"export ODIN3_HEAVY_CGROUP={self.cgroup_file(f'{PREFIX}0.slice')}; "
                 f"{BASH} {HEAVY_RUN} echo inner && "
                 f"{{ {BASH} {BUILD_LOCKED} --preset debug; echo build=$?; }}")
        start = time.monotonic()
        run = self.run_tool(HEAVY_RUN, BASH, "-c", inner, ODIN3_HEAVY_OK="1",
                            ODIN3_HEAVY_WAIT="60")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "inner\nbuild=2\n")
        self.assertLess(time.monotonic() - start, 30.0)  # a wait on its own lock lasts 60 s

    def test_python_driver_must_pass_fd9(self) -> None:
        """A Python driver's nested step runs in place with pass_fds=(9,), and fails at once
        (exit 2, not a 4 h wait) without it, since subprocess closes fd 9 by default."""
        self.real_flock()
        driver = ("import subprocess, sys\n"
                  "for fds in ((9,), ()):\n"
                  f"    step = subprocess.run([{BASH!r}, {str(HEAVY_RUN)!r}, 'echo', 'step'],"
                  " pass_fds=fds)\n"
                  "    print(f'rc={step.returncode}', flush=True)\n")
        inner = (f"export ODIN3_HEAVY_CGROUP={self.cgroup_file(f'{PREFIX}0.slice')}; "
                 f"exec python3 -I -c \"$1\"")
        start = time.monotonic()
        run = self.run_tool(HEAVY_RUN, BASH, "-c", inner, "job", driver, ODIN3_HEAVY_OK="1",
                            ODIN3_HEAVY_WAIT="60")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "step\nrc=0\nrc=2\n")
        self.assertIn("pass_fds=(9,)", run.stderr)
        self.assertLess(time.monotonic() - start, 30.0)  # a wait on its own lock lasts 60 s


class RealLockTest(FakeTools):
    """The real flock(1): the job holds the lock for its whole life, the wrapper's fate aside."""

    def setUp(self) -> None:
        super().setUp()
        self.real_flock()

    def test_lock_is_held_by_the_job_and_released_after(self) -> None:
        probe = (f"flock -n -x {self.slot0} true && echo free0 || echo held0; "
                 f"flock -n -x {self.slot1} true && echo free1 || echo held1")
        run = self.run_tool(HEAVY_RUN, BASH, "-c", probe, ODIN3_HEAVY_OK="1")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "held0\nfree1\n")  # one slot only
        self.assertTrue(_lock_free(self.slot0) and _lock_free(self.slot1))

    def test_lock_survives_a_sigkilled_wrapper(self) -> None:
        pid_file = self.dir / "job.pid"
        go = self.dir / "go"
        job = f"echo $$ >{pid_file}; while [ ! -e {go} ]; do sleep 0.05; done"
        wrapper = subprocess.Popen([BASH, str(HEAVY_RUN), BASH, "-c", job],
                                   env={**self.env, "ODIN3_HEAVY_OK": "1"},
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            _wait_for(pid_file.exists, "the job to start")
            self.assertFalse(_lock_free(self.slot0))
            wrapper.send_signal(signal.SIGKILL)
            wrapper.wait(timeout=30)
            self.assertFalse(_lock_free(self.slot0))  # the running job still holds it
        finally:
            go.touch()
            wrapper.kill()
            wrapper.wait(timeout=30)
        _wait_for(lambda: _lock_free(self.slot0), "the lock to be released")

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
        self.assertEqual(run.stderr, "heavy-run: lock busy for 1 s; skipped\n")  # the legacy lock

    def test_both_slots_busy_skips(self) -> None:
        self.heavy.mkdir()
        holders = [os.open(f, os.O_WRONLY | os.O_CREAT | os.O_APPEND)
                   for f in (self.slot0, self.slot1)]
        try:
            for fd in holders:
                fcntl.flock(fd, fcntl.LOCK_EX)
            run = self.run_tool(HEAVY_RUN, "echo", "never", ODIN3_HEAVY_OK="1",
                                ODIN3_HEAVY_WAIT="1")
        finally:
            for fd in holders:
                os.close(fd)
        self.assertEqual(run.returncode, SKIP)
        self.assertEqual(run.stdout, "")
        self.assertEqual(run.stderr, "heavy-run: lock busy for 1 s; skipped\n")
        self.assertEqual(self.calls(), [])

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
            f"flock -s -n {self.slot0} true | flag=no fd9=none",
            f"flock -s -n {self.slot1} true | flag=no fd9=none",
            f"flock -s 9 | flag=no fd9={self.lock}",
            f"flock -s 8 | flag=no fd9={self.lock}",
            f"flock -s 7 | flag=no fd9={self.lock}",
            "cmake --build --preset debug"])


class TwoSlotsTest(FakeTools):
    """Real flock(1), fake systemd-run: two jobs share the machine, a third waits, --solo takes it
    all, builds and heavy jobs exclude each other. Each job writes its slot to <tag>.slot, then
    waits for <tag>.go; the waits here are short and bounded."""

    def setUp(self) -> None:
        super().setUp()
        self.real_flock()
        self.procs: list[subprocess.Popen[str]] = []

    def tearDown(self) -> None:
        for go in self.dir.glob("*.slot"):
            (self.dir / (go.stem + ".go")).touch()
        for proc in self.procs:
            proc.kill()
            proc.wait(timeout=30)
        super().tearDown()

    def start(self, tag: str, *flags: str, **env: str) -> subprocess.Popen[str]:
        job = (f'echo "$ODIN3_HEAVY_SLOT" >{self.dir}/{tag}.slot; '
               f"while [ ! -e {self.dir}/{tag}.go ]; do sleep 0.05; done")
        proc = subprocess.Popen(
            [BASH, str(HEAVY_RUN), *flags, BASH, "-c", job], text=True,
            env={**self.env, "ODIN3_HEAVY_OK": "1", "ODIN3_HEAVY_XL_OK": "1", **env},
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(proc)
        return proc

    def started(self, tag: str) -> bool:
        return (self.dir / f"{tag}.slot").exists()

    def slot_of(self, tag: str) -> str:
        return (self.dir / f"{tag}.slot").read_text().strip()

    def release(self, tag: str) -> None:
        (self.dir / f"{tag}.go").touch()

    def finish(self, proc: subprocess.Popen[str]) -> str:
        _, err = proc.communicate(timeout=30)
        self.assertEqual(proc.returncode, 0, err)
        return err

    def test_two_jobs_run_concurrently_in_different_slots(self) -> None:
        a, b = self.start("a"), self.start("b")
        _wait_for(lambda: self.started("a") and self.started("b"), "both jobs to start")
        self.assertEqual({self.slot_of("a"), self.slot_of("b")}, {"0", "1"})
        self.assertFalse(_lock_free(self.slot0) or _lock_free(self.slot1))
        self.release("a")
        self.release("b")
        self.finish(a)
        self.finish(b)
        self.assertTrue(_lock_free(self.slot0) and _lock_free(self.slot1))

    def test_a_third_job_waits_for_the_first_free_slot(self) -> None:
        a, b = self.start("a"), self.start("b")
        _wait_for(lambda: self.started("a") and self.started("b"), "both jobs to start")
        c = self.start("c", ODIN3_HEAVY_WAIT="60")
        time.sleep(1.0)
        self.assertFalse(self.started("c"))
        self.assertIsNone(c.poll())
        self.assertTrue(list(self.heavy.glob("odin3-heavy.queued.*")))  # waiting: flagged
        self.release("b")
        _wait_for(lambda: self.started("c"), "the third job to take the freed slot")
        self.assertEqual(self.slot_of("c"), self.slot_of("b"))
        self.assertFalse((self.heavy / f"odin3-heavy.queued.{c.pid}").exists())
        for tag, proc in (("a", a), ("b", b), ("c", c)):
            self.release(tag)
            self.finish(proc)

    def test_a_third_job_times_out_with_77(self) -> None:
        a, b = self.start("a"), self.start("b")
        _wait_for(lambda: self.started("a") and self.started("b"), "both jobs to start")
        c = self.start("c", ODIN3_HEAVY_WAIT="1")
        _, err = c.communicate(timeout=30)
        self.assertEqual(c.returncode, SKIP)
        self.assertEqual(err, "heavy-run: lock busy for 1 s; skipped\n")
        self.release("a")
        self.release("b")
        self.finish(a)
        self.finish(b)

    def test_solo_waits_for_both_slots_and_blocks_new_jobs(self) -> None:
        a, b = self.start("a"), self.start("b")
        _wait_for(lambda: self.started("a") and self.started("b"), "both jobs to start")
        s = self.start("s", "--solo", ODIN3_HEAVY_WAIT="60")
        time.sleep(1.0)
        self.assertFalse(self.started("s"))
        self.release("a")
        time.sleep(1.0)
        self.assertFalse(self.started("s"))  # one slot free is not enough
        self.release("b")
        _wait_for(lambda: self.started("s"), "the solo job to start")
        self.assertEqual(self.slot_of("s"), "solo")
        self.assertFalse(_lock_free(self.slot0) or _lock_free(self.slot1))  # both held
        d = self.start("d", ODIN3_HEAVY_WAIT="1")  # a new job finds no slot
        _, err = d.communicate(timeout=30)
        self.assertEqual(d.returncode, SKIP)
        self.assertEqual(err, "heavy-run: lock busy for 1 s; skipped\n")
        self.assertFalse(self.started("d"))
        self.release("s")
        self.finish(s)
        self.finish(a)
        self.finish(b)

    def test_a_build_blocks_and_is_blocked(self) -> None:
        cmake = self.dir / "slow-cmake" / "cmake"
        cmake.parent.mkdir()
        _write_exec(cmake, f"#!{{bash}}\ntouch {self.dir}/build.started\n"
                           f"while [ ! -e {self.dir}/build.go ]; do sleep 0.05; done\n")
        a = self.start("a")
        _wait_for(lambda: self.started("a"), "the job to start")
        build = subprocess.Popen([BASH, str(BUILD_LOCKED), "--preset", "debug"], text=True,
                                 env={**self.env, "PATH": f"{cmake.parent}:{self.env['PATH']}"},
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(build)
        time.sleep(1.0)
        self.assertFalse((self.dir / "build.started").exists())  # a job on one slot blocks it
        self.release("a")
        _wait_for((self.dir / "build.started").exists, "the build to start")
        self.finish(a)
        b = self.start("b", ODIN3_HEAVY_WAIT="60")  # the running build blocks a job (both slots)
        time.sleep(1.0)
        self.assertFalse(self.started("b"))
        self.assertTrue(list(self.heavy.glob("odin3-heavy.queued.*")))
        (self.dir / "build.go").touch()
        _, err = build.communicate(timeout=30)
        self.assertEqual(build.returncode, 0, err)
        _wait_for(lambda: self.started("b"), "the job to start after the build")
        self.release("b")
        self.finish(b)

    def test_a_queued_job_holds_off_new_builds(self) -> None:
        a, b = self.start("a"), self.start("b")
        _wait_for(lambda: self.started("a") and self.started("b"), "both jobs to start")
        c = self.start("c", ODIN3_HEAVY_WAIT="60")
        _wait_for(lambda: bool(list(self.heavy.glob("odin3-heavy.queued.*"))), "the queue flag")
        build = subprocess.Popen([BASH, str(BUILD_LOCKED), "--preset", "debug"], text=True,
                                 env={**self.env, "ODIN3_HEAVY_POLL": "0.1"},
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.procs.append(build)
        time.sleep(1.0)
        self.assertIsNone(build.poll())
        self.assertFalse(any(c.startswith("cmake") for c in self.calls()))  # no build yet
        for tag, proc in (("a", a), ("b", b), ("c", c)):
            self.release(tag)
            self.finish(proc)
        _, err = build.communicate(timeout=30)
        self.assertEqual(build.returncode, 0, err)
        self.assertEqual([c for c in self.calls() if c.startswith("cmake")],
                         ["cmake --build --preset debug"])

    def test_nested_call_reuses_its_slot(self) -> None:
        """A job that calls heavy-run (in its slice, fd 9 inherited) neither takes nor waits for
        a slot, though the other slot is busy and the wait would last 60 s."""
        b = self.start("b")
        _wait_for(lambda: self.started("b"), "the other job to start")
        cgroup = self.dir / "cgroup-in-slot"
        cgroup.write_text(CGROUP_IN.format(slice=f"{PREFIX}0.slice"))
        inner = (f"export ODIN3_HEAVY_CGROUP={cgroup}; {BASH} {HEAVY_RUN} "
                 f'{BASH} -c \'echo "$ODIN3_HEAVY_SLICE"\'')
        start = time.monotonic()
        run = self.run_tool(HEAVY_RUN, BASH, "-c", inner, ODIN3_HEAVY_OK="1",
                            ODIN3_HEAVY_WAIT="60")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, f"{PREFIX}0.slice\n")  # the faked cgroup names the slice
        self.assertEqual(run.stderr.count("heavy-run: slot"), 1)  # the outer run's line only
        self.assertLess(time.monotonic() - start, 30.0)
        self.release("b")
        self.finish(b)

    def test_old_binary_exclusive_lock_still_serialises(self) -> None:
        """An old heavy-run (exclusive on odin3-heavy.lock) holds off a new job; new jobs share
        the legacy lock with each other (test_two_jobs_...)."""
        self.heavy.mkdir()
        holder = os.open(self.lock, os.O_WRONLY | os.O_CREAT | os.O_APPEND)
        try:
            fcntl.flock(holder, fcntl.LOCK_EX)
            a = self.start("a", ODIN3_HEAVY_WAIT="60")
            time.sleep(1.0)
            self.assertFalse(self.started("a"))
        finally:
            os.close(holder)
        _wait_for(lambda: self.started("a"), "the job to start after the old holder")
        self.release("a")
        self.finish(a)


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
        prefix = f"odin3-htest{os.getpid()}"  # never the machine's slices
        slice_name = f"{prefix}0.slice"
        try:
            with tempfile.TemporaryDirectory() as heavy:  # never the machine's locks
                run = subprocess.run(
                    [BASH, str(HEAVY_RUN), "true"],
                    env={**env, "ODIN3_HEAVY_OK": "1", "ODIN3_HEAVY_DIR": heavy,
                         "ODIN3_HEAVY_SLICE_PREFIX": prefix},
                    capture_output=True, text=True, timeout=120, check=False)
            if run.returncode == SKIP:
                self.skipTest(run.stderr.strip())
            self.assertEqual(run.returncode, 0, run.stderr)
            show = subprocess.run(["systemctl", "--user", "show", slice_name, "-p", "MemoryMax"],
                                  capture_output=True, text=True, timeout=30, check=False)
            self.assertEqual(show.stdout.strip(), "MemoryMax=6442450944")
        finally:
            subprocess.run(["systemctl", "--user", "stop", slice_name], capture_output=True,
                           timeout=30, check=False)


if __name__ == "__main__":
    unittest.main()

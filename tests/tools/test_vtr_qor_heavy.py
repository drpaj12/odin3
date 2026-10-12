"""Tests for tools/vtr-qor/qor_heavy.py (3H plan Task 1: T6 and T4's safe_rmtree).

Every case uses temp files under /tmp/claude-1000, fake executables and an injected lock: nothing
touches the real heavy lock, the real cgroups or tools/heavy-run. Fakes reach the module through
its seams (mock.patch.object) or keyword parameters, never through the environment (fix round 1).
"""

from __future__ import annotations

import contextlib
import fcntl
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

import qor_heavy
from qor_heavy import CAP_XL_BYTES, HeavyError, ScopeInfo

from . import helpers

SLICE_DIRS = "user.slice/user-1000.slice/user@1000.service/odin3.slice"
SCOPE = "odin3-heavy-4242-1760000000000000000.scope"
CAP_6G = "6442450944"
CAP_9G = "9663676416"
GIB = 1024**3
DF_72 = f"Mounted on       Avail\n/mnt/c     {72 * GIB}\n"
DF_ARGV = [qor_heavy.DF_BIN, "-B1", "--output=target,avail", "/mnt/c"]
XL_NESTED = "--xl inside odin3-heavy.slice (6G): run the xl job on its own"


class HeavyTestBase(unittest.TestCase):
    def setUp(self) -> None:
        qor_heavy.HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=qor_heavy.HEAVY_DIR, prefix="qor-heavy-test-")
        self.tmp = Path(self._tmp.name)
        self.lock = self.tmp / "odin3-heavy.lock"
        self.lock.touch()
        self.record = self.tmp / "record.json"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    # -- fakes -------------------------------------------------------------------------------

    def cgroup_tree(self, slice_name: str = "odin3-heavy.slice", *, mem: str = CAP_6G,
                    swap: str = "0") -> tuple[Path, Path, Path]:
        """A fake /sys/fs/cgroup with heavy-run's slice and scope; returns (root, slice, scope)
        and points self.self_cgroup at the scope."""
        root = self.tmp / "cg"
        slice_dir = root / SLICE_DIRS / slice_name
        scope = slice_dir / SCOPE.replace("odin3-heavy", slice_name.removesuffix(".slice"))
        scope.mkdir(parents=True, exist_ok=True)
        for directory, mem_max, swap_max in ((scope, "max", "max"), (slice_dir, mem, swap)):
            (directory / "memory.max").write_text(mem_max + "\n")
            (directory / "memory.swap.max").write_text(swap_max + "\n")
        self.self_cgroup = self.tmp / "self-cgroup"
        self.self_cgroup.write_text(f"0::/{scope.relative_to(root)}\n")
        return root, slice_dir, scope

    def fd9(self, target: Path) -> Path:
        link = self.tmp / "fd9"
        if link.is_symlink():
            link.unlink()
        link.symlink_to(target)
        return link

    def fake_heavy_run(self, code: int = 0, *, take_lock: bool = False) -> Path:
        """A fake tools/heavy-run: optionally waits for the lock, then dumps its argv and
        environment to self.record and exits with `code`."""
        fake = self.tmp / "heavy-run"
        lock_wait = (
            f"fd = os.open({str(self.lock)!r}, os.O_WRONLY | os.O_APPEND)\n"
            "fcntl.flock(fd, fcntl.LOCK_EX)\n" if take_lock else ""
        )
        fake.write_text(
            f"#!{sys.executable} -I\n"
            "import fcntl, json, os, sys\n"
            f"{lock_wait}"
            f"with open({str(self.record)!r}, 'w') as f:\n"
            "    json.dump({'argv': sys.argv, 'env': dict(os.environ)}, f)\n"
            f"sys.exit({code})\n"
        )
        fake.chmod(0o755)
        return fake

    def caller_env(self, **extra: str) -> dict[str, str]:
        env = {
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "LC_ALL": "C.UTF-8",
            "ODIN3_HEAVY_DIR": str(self.tmp),
            "ODIN3_QOR_TEST_MARKER": "caller",
        }
        env.update(extra)
        return env

    def inside_env(self, slice_name: str = "odin3-heavy.slice", **extra: str) -> dict[str, str]:
        return self.caller_env(ODIN3_HEAVY_SLICE=slice_name, **extra)

    def machine(self, root: Path) -> contextlib.ExitStack:
        """Point the module seams (never the environment) at the fake cgroup tree, a fake fd 9
        holding self.lock, and self.lock as HEAVY_LOCK."""
        stack = contextlib.ExitStack()
        for name, value in (("SELF_CGROUP", self.self_cgroup), ("CGROUP_ROOT", root),
                            ("FD9", self.fd9(self.lock)), ("HEAVY_LOCK", self.lock)):
            stack.enter_context(mock.patch.object(qor_heavy, name, value))
        return stack

    def hooks_env(self, root: Path, **extra: str) -> dict[str, str]:
        """Every retired ODIN3_QOR_* hook set to a passing fake, plus ODIN3_HEAVY_SLICE."""
        df = self.tmp / "fake-df"
        df.write_text("#!/bin/sh\nprintf 'Avail\\n  999999\\n'\n")
        df.chmod(0o755)
        return self.inside_env(
            ODIN3_QOR_SELF_CGROUP=str(self.self_cgroup), ODIN3_QOR_CGROUP_ROOT=str(root),
            ODIN3_QOR_FD9=str(self.fd9(self.lock)), ODIN3_QOR_DF_CMD=str(df), **extra)

    def hold_lock(self) -> int:
        fd = os.open(self.lock, os.O_WRONLY | os.O_APPEND)
        fcntl.flock(fd, fcntl.LOCK_EX)
        return fd

    def assert_heavy(self, ctx: unittest.case._AssertRaisesContext[HeavyError], code: int,
                     message: str) -> None:
        self.assertEqual(ctx.exception.code, code)
        self.assertEqual(ctx.exception.message, message)


class ReexecTest(HeavyTestBase):
    """T6: the re-exec through tools/heavy-run and the nesting evidence."""

    def test_reexec_argv(self) -> None:
        script = qor_heavy.REPO_ROOT / "tools/vtr-qor/vtr_qor.py"
        heavy_run = str(qor_heavy.REPO_ROOT / "tools/heavy-run")
        tail = [sys.executable, "-I", str(script), "run", "--leg", "P"]
        self.assertEqual(qor_heavy.reexec_argv(["run", "--leg", "P"], script), [heavy_run, *tail])
        self.assertEqual(qor_heavy.reexec_argv(["run", "--leg", "P"], script, xl=True),
                         [heavy_run, "--xl", *tail])

    def test_reexec_env(self) -> None:
        fake = self.fake_heavy_run(0)
        script = self.tmp / "tool.py"
        env = self.caller_env(ODIN3_HEAVY_OK="1")
        with mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                self.assertRaises(SystemExit) as ctx:
            qor_heavy.reexec_under_lock(["a", "b"], script=script, lock=self.lock, env=env)
        self.assertEqual(ctx.exception.code, 0)
        seen = json.loads(self.record.read_text())
        self.assertEqual(seen["env"], env)
        self.assertEqual(seen["argv"][1:], [sys.executable, "-I", str(script), "a", "b"])

        self.record.unlink()
        no_ok = self.caller_env()
        with mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.reexec_under_lock(["a"], script=script, lock=self.lock, env=no_ok)
        self.assert_heavy(err, 2, "heavy runs are the controller's: set ODIN3_HEAVY_OK=1")
        self.assertFalse(self.record.exists())

        with mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.reexec_under_lock(["a"], script=script, xl=True, lock=self.lock, env=env)
        self.assert_heavy(
            err, 2, "--xl needs ODIN3_HEAVY_XL_OK=1 (an idle machine; the controller starts it)")
        self.assertFalse(self.record.exists())

    def test_reexec_inside(self) -> None:
        root, slice_dir, scope = self.cgroup_tree()
        fake = self.fake_heavy_run(0)
        env = self.inside_env()
        with self.machine(root), mock.patch.object(qor_heavy, "HEAVY_RUN", fake):
            info = qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock,
                                               env=env)
        self.assertEqual(info, ScopeInfo(path=slice_dir, leaf=scope, memory_max=int(CAP_6G),
                                         swap_zero=True))
        self.assertFalse(self.record.exists())

        def inside(**kw: Path) -> bool:
            args = {"self_cgroup": self.self_cgroup, "fd_link": self.tmp / "fd9",
                    "lock": self.lock, **kw}
            return qor_heavy.inside_heavy_run(env, **args)

        self.assertTrue(inside())
        self.assertFalse(inside(fd_link=self.tmp / "no-fd9"))  # the variable alone
        other = self.tmp / "other.lock"
        other.touch()
        self.assertFalse(inside(fd_link=self.fd9(other)))  # fd 9 is not the heavy lock
        self.fd9(self.lock)
        outside = self.tmp / "outside-cgroup"
        outside.write_text("0::/user.slice/user-1000.slice/session-3.scope\n")
        self.assertFalse(inside(self_cgroup=outside))  # a cgroup outside the slice
        self.assertFalse(qor_heavy.inside_heavy_run(
            {k: v for k, v in env.items() if k != "ODIN3_HEAVY_SLICE"},
            self_cgroup=self.self_cgroup, fd_link=self.tmp / "fd9", lock=self.lock))
        # With the evidence incomplete, reexec takes the outside path (and wants ODIN3_HEAVY_OK).
        with self.machine(root), mock.patch.object(qor_heavy, "FD9", self.tmp / "no-fd9"), \
                mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock, env=env)
        self.assert_heavy(err, 2, "heavy runs are the controller's: set ODIN3_HEAVY_OK=1")
        self.assertFalse(self.record.exists())

    def test_reexec_inside_refusals(self) -> None:
        """Inside a run, a failing cap or --xl nested in the 6G slice refuses; nothing runs."""
        fake = self.fake_heavy_run(0)
        root, _, _ = self.cgroup_tree(swap="max")
        with self.machine(root), mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock,
                                        env=self.inside_env(ODIN3_HEAVY_OK="1"))
        self.assert_heavy(err, 2, "not inside a capped scope (memory.max 6442450944, "
                                  "memory.swap.max max); run through the heavy wrapper")
        root, _, _ = self.cgroup_tree()
        env = self.inside_env(ODIN3_HEAVY_OK="1", ODIN3_HEAVY_XL_OK="1")
        with self.machine(root), mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", xl=True,
                                        lock=self.lock, env=env)
        self.assert_heavy(err, 2, XL_NESTED)
        self.assertFalse(self.record.exists())
        root, slice_dir, _ = self.cgroup_tree("odin3-heavyxl.slice", mem=CAP_9G)
        with self.machine(root):
            info = qor_heavy.reexec_under_lock(["x"], xl=True, lock=self.lock,
                                               env=self.inside_env("odin3-heavyxl.slice"))
        self.assertEqual((info.path, info.memory_max), (slice_dir, int(CAP_9G)))

    def test_require_heavy(self) -> None:
        with mock.patch.dict(os.environ, self.caller_env(), clear=True), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.require_heavy()
        self.assert_heavy(
            err, 2, "not inside a heavy run: the controller starts it through tools/heavy-run")
        root, slice_dir, _ = self.cgroup_tree()
        with self.machine(root), mock.patch.dict(os.environ, self.inside_env(), clear=True):
            self.assertEqual(qor_heavy.require_heavy().path, slice_dir)
            with self.assertRaises(HeavyError) as err:
                qor_heavy.require_heavy(xl=True)
            self.assert_heavy(err, 2, XL_NESTED)
        root, _, _ = self.cgroup_tree(mem="8589934592")
        with self.machine(root), mock.patch.dict(os.environ, self.inside_env(), clear=True), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.require_heavy()
        self.assert_heavy(err, 2, "not inside a capped scope (memory.max 8589934592, "
                                  "memory.swap.max max); run through the heavy wrapper")

    def test_cli_require_heavy(self) -> None:
        root, _, _ = self.cgroup_tree()
        with self.machine(root), mock.patch.dict(os.environ, self.inside_env(), clear=True):
            self.assertEqual(helpers.run_main(qor_heavy.main, ["require-heavy"]), (0, "", ""))
            self.assertEqual(helpers.run_main(qor_heavy.main, ["require-heavy", "--xl"]),
                             (2, "", f"vtr-qor: {XL_NESTED}\n"))
        with mock.patch.dict(os.environ, self.caller_env(), clear=True):
            self.assertEqual(helpers.run_main(qor_heavy.main, ["require-heavy"])[0], 3)
        root, _, _ = self.cgroup_tree(swap="max")
        with self.machine(root), mock.patch.dict(os.environ, self.inside_env(), clear=True):
            code, _, err = helpers.run_main(qor_heavy.main, ["require-heavy"])
        self.assertEqual(code, 2)
        self.assertEqual(err, "vtr-qor: not inside a capped scope (memory.max 6442450944, "
                              "memory.swap.max max); run through the heavy wrapper\n")
        root, _, _ = self.cgroup_tree("odin3-heavyxl.slice", mem=CAP_9G)
        with self.machine(root), \
                mock.patch.dict(os.environ, self.inside_env("odin3-heavyxl.slice"), clear=True):
            self.assertEqual(helpers.run_main(qor_heavy.main, ["require-heavy"])[0], 2)
            self.assertEqual(helpers.run_main(qor_heavy.main, ["require-heavy", "--xl"])[0], 0)

    def test_cli_script_outside(self) -> None:
        script = qor_heavy.REPO_ROOT / "tools/vtr-qor/qor_heavy.py"
        proc = subprocess.run([sys.executable, "-I", str(script), "require-heavy"],
                              env=self.caller_env(), capture_output=True, text=True, check=False)
        self.assertEqual((proc.returncode, proc.stdout, proc.stderr), (3, "", ""))


class LockTest(HeavyTestBase):
    """The lock probe before tools/heavy-run (plan point (a))."""

    def test_lock_busy_no_wait(self) -> None:
        fake = self.fake_heavy_run(0)
        fd = self.hold_lock()
        try:
            with mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                    self.assertRaises(HeavyError) as err:
                qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", no_wait=True,
                                            lock=self.lock, env=self.caller_env(ODIN3_HEAVY_OK="1"))
        finally:
            os.close(fd)
        self.assert_heavy(err, 4, f"heavy lock {self.lock} is busy (--no-wait)")
        self.assertFalse(self.record.exists())

    def test_lock_wait_message(self) -> None:
        fake = self.fake_heavy_run(0, take_lock=True)
        fd = self.hold_lock()
        timer = threading.Timer(0.2, os.close, args=(fd,))
        timer.start()
        stderr = io.StringIO()
        try:
            with mock.patch.object(qor_heavy, "HEAVY_RUN", fake), redirect_stderr(stderr), \
                    self.assertRaises(SystemExit) as ctx:
                qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock,
                                            env=self.caller_env(ODIN3_HEAVY_OK="1"))
        finally:
            timer.join()
        self.assertEqual(ctx.exception.code, 0)
        self.assertEqual(stderr.getvalue(), f"vtr-qor: waiting for the heavy lock {self.lock}\n")
        self.assertTrue(self.record.exists())

    def test_heavy_run_77(self) -> None:
        env = self.caller_env(ODIN3_HEAVY_OK="1")
        with mock.patch.object(qor_heavy, "HEAVY_RUN", self.fake_heavy_run(77)), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock, env=env)
        self.assert_heavy(err, 2, "cannot apply the memory cap (tools/heavy-run exited 77; "
                                  "see its message above)")
        self.record.unlink()
        with mock.patch.object(qor_heavy, "HEAVY_RUN", self.fake_heavy_run(3)), \
                self.assertRaises(SystemExit) as ctx:
            qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock, env=env)
        self.assertEqual(ctx.exception.code, 3)
        self.record.unlink()
        fd = self.hold_lock()
        try:
            with mock.patch.object(qor_heavy, "HEAVY_RUN", self.fake_heavy_run(0)), \
                    self.assertRaises(HeavyError) as err:
                qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", no_wait=True,
                                            lock=self.lock, env=env)
        finally:
            os.close(fd)
        self.assertEqual(err.exception.code, 4)
        self.assertFalse(self.record.exists())
        missing = self.tmp / "no-heavy-run"  # before 2E-1 merges: no fallback (point (g))
        with mock.patch.object(qor_heavy, "HEAVY_RUN", missing), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock, env=env)
        self.assert_heavy(err, 2, f"cannot apply the memory cap (cannot run {missing}: "
                                  "No such file or directory)")


    def test_reexec_signal_and_inherited_env(self) -> None:
        """A child killed by a signal exits 128 + N (M6d); env=None passes os.environ (M6f)."""
        fake = self.tmp / "heavy-run"
        fake.write_text(f"#!{sys.executable} -I\nimport json, os, signal\n"
                        f"open({str(self.record)!r}, 'w').write(json.dumps(dict(os.environ)))\n"
                        "os.kill(os.getpid(), signal.SIGTERM)\n")
        fake.chmod(0o755)
        env = self.caller_env(ODIN3_HEAVY_OK="1")
        with mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                self.assertRaises(SystemExit) as ctx:
            qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock)
        self.assertEqual(ctx.exception.code, 143)
        self.assertEqual(json.loads(self.record.read_text()), env)


class RulingTest(HeavyTestBase):
    """The controller's ruling (fix round 1): with every retired ODIN3_QOR_* hook set to a passing
    fake, plus ODIN3_HEAVY_SLICE and no fd 9, the production paths ignore the hooks."""

    def setUp(self) -> None:
        super().setUp()
        try:
            held = os.path.realpath(os.readlink("/proc/self/fd/9"))
        except OSError:
            held = ""
        if held == os.path.realpath(qor_heavy.HEAVY_LOCK):
            self.skipTest("this test process really runs inside a heavy run (fd 9 is the lock)")
        root, _, _ = self.cgroup_tree()
        self.root = root

    def test_require_heavy_ignores_hooks(self) -> None:
        with mock.patch.dict(os.environ, self.hooks_env(self.root), clear=True), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.require_heavy()
        self.assert_heavy(
            err, 2, "not inside a heavy run: the controller starts it through tools/heavy-run")
        with mock.patch.dict(os.environ, self.hooks_env(self.root), clear=True):
            self.assertEqual(helpers.run_main(qor_heavy.main, ["require-heavy"])[0], 3)

    def test_reexec_ignores_hooks(self) -> None:
        fake = self.fake_heavy_run(0)
        env = self.hooks_env(self.root, ODIN3_HEAVY_OK="1")
        with mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                self.assertRaises(SystemExit) as ctx:
            qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock, env=env)
        self.assertEqual(ctx.exception.code, 0)
        self.assertEqual(json.loads(self.record.read_text())["env"], env)  # re-executed
        self.record.unlink()
        with mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(qor_heavy, "HEAVY_RUN", fake), \
                self.assertRaises(SystemExit):
            qor_heavy.reexec_under_lock(["x"], script=self.tmp / "t.py", lock=self.lock)
        self.assertTrue(self.record.exists())

    def test_disk_free_ignores_hook(self) -> None:
        done = subprocess.CompletedProcess(args=[], returncode=0, stdout=DF_72)
        with mock.patch.dict(os.environ, self.hooks_env(self.root), clear=True), \
                mock.patch("qor_heavy.subprocess.run", return_value=done) as run:
            self.assertEqual(qor_heavy.disk_free_gib(), 72)
        self.assertEqual(run.call_args.args[0], DF_ARGV)
        if Path("/mnt/c").is_dir():  # the real df on /mnt/c, never the fake's 999999
            with mock.patch.dict(os.environ, self.hooks_env(self.root), clear=True):
                self.assertNotEqual(qor_heavy.disk_free_gib(), 999999)


class ScopeTest(HeavyTestBase):
    """verify_scope's ancestor walk (plan point (b), finding 26)."""

    def verify(self, limit: int = qor_heavy.CAP_BYTES, *, root: Path) -> ScopeInfo:
        return qor_heavy.verify_scope(limit, self_cgroup=self.self_cgroup, cgroup_root=root)

    def test_verify_scope_ancestor(self) -> None:
        root, slice_dir, scope = self.cgroup_tree()
        self.assertEqual(
            self.self_cgroup.read_text(),
            f"0::/{SLICE_DIRS}/odin3-heavy.slice/odin3-heavy-4242-1760000000000000000.scope\n")
        self.assertEqual(self.verify(root=root), ScopeInfo(
            path=slice_dir, leaf=scope, memory_max=6442450944, swap_zero=True))
        root, slice_dir, scope = self.cgroup_tree("odin3-heavyxl.slice", mem=CAP_9G)
        with self.assertRaises(HeavyError) as err:
            self.verify(root=root)
        self.assertEqual(err.exception.code, 2)
        self.assertEqual(self.verify(CAP_XL_BYTES, root=root), ScopeInfo(
            path=slice_dir, leaf=scope, memory_max=9663676416, swap_zero=True))

    def test_verify_scope_refusals(self) -> None:
        def refusal(mem: str) -> str:
            return (f"not inside a capped scope (memory.max {mem}, memory.swap.max max); "
                    "run through the heavy wrapper")

        root, _, _ = self.cgroup_tree(mem="max", swap="max")
        with self.assertRaises(HeavyError) as err:
            self.verify(root=root)
        self.assert_heavy(err, 2, refusal("max"))
        root, _, _ = self.cgroup_tree(mem="8589934592")
        with self.assertRaises(HeavyError) as err:
            self.verify(root=root)
        self.assert_heavy(err, 2, refusal("8589934592"))
        root, _, _ = self.cgroup_tree(swap="max")
        with self.assertRaises(HeavyError) as err:
            self.verify(root=root)
        self.assert_heavy(err, 2, refusal(CAP_6G))
        root, _, _ = self.cgroup_tree(mem=CAP_9G)
        self.assertEqual(self.verify(CAP_XL_BYTES, root=root).memory_max, int(CAP_9G))
        self.self_cgroup.write_text("1:name=systemd:/x\n")  # no cgroup v2 entry
        with self.assertRaises(HeavyError) as err:
            self.verify(root=root)
        self.assertEqual(err.exception.code, 2)

    def test_oom_and_peak(self) -> None:
        root, slice_dir, scope = self.cgroup_tree()
        info = self.verify(root=root)
        self.assertEqual(qor_heavy.oom_kills(info), 0)
        (slice_dir / "memory.events").write_text("oom 2\noom_kill 3\n")
        self.assertEqual(qor_heavy.oom_kills(info), 3)
        (slice_dir / "memory.peak").write_text("4294967296\n")
        self.assertIsNone(qor_heavy.scope_peak_mib(info))
        (scope / "memory.peak").write_text("1073741824\n")
        self.assertEqual(qor_heavy.scope_peak_mib(info), 1024)


class ReaderTest(HeavyTestBase):
    """The machine readers for admission (spec §7.3)."""

    def fake_df(self, body: str) -> str:
        fake = self.tmp / "df"
        fake.write_text(f"#!/bin/sh\n{body}\n")
        fake.chmod(0o755)
        return str(fake)

    def disk_free(self, body: str) -> int:
        with mock.patch.object(qor_heavy, "DF_CMD", (self.fake_df(body),)):
            return qor_heavy.disk_free_gib()

    def test_disk_free(self) -> None:
        def df_says(target: str, avail: str) -> str:
            return f"printf 'Mounted on       Avail\\n{target}     {avail}\\n'"

        self.assertEqual(self.disk_free(df_says("/mnt/c", str(117 * GIB))), 117)
        self.assertEqual(self.disk_free(df_says("/mnt/c", str(10 * GIB - 1))), 9)  # floored
        for body in ("exit 1", df_says("/mnt/c", "lots"), ":", r"printf 'Mounted on  Avail\n'",
                     df_says("/", str(900 * GIB)),  # /mnt/c not mounted: df reports /
                     df_says("/mnt/c/x", str(900 * GIB))):
            with self.assertRaises(HeavyError) as err:
                self.disk_free(body)
            self.assert_heavy(err, 5, "cannot read free space of /mnt/c")
        with mock.patch.object(qor_heavy, "DF_CMD", (str(self.tmp / "no-such-df"),)), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.disk_free_gib()
        self.assertEqual(err.exception.code, 5)

    def test_disk_free_argv(self) -> None:
        """The production argv is df on /mnt/c by absolute path (M6c, M3)."""
        self.assertIn(qor_heavy.DF_BIN, ("/usr/bin/df", "/bin/df"))
        done = subprocess.CompletedProcess(args=[], returncode=0, stdout=DF_72)
        with mock.patch("qor_heavy.subprocess.run", return_value=done) as run:
            self.assertEqual(qor_heavy.disk_free_gib(), 72)
        self.assertEqual(run.call_args.args[0], DF_ARGV)

    def test_mem_available(self) -> None:
        meminfo = self.tmp / "meminfo"
        meminfo.write_text("MemTotal:       24576000 kB\nMemAvailable:   19922944 kB\n")
        self.assertEqual(qor_heavy.mem_available_gib(meminfo), 19.0)
        meminfo.write_text("MemTotal:       24576000 kB\n")
        with self.assertRaises(HeavyError) as err:
            qor_heavy.mem_available_gib(meminfo)
        self.assertEqual(err.exception.code, 2)


class DeletionTest(HeavyTestBase):
    """safe_rmtree (spec §7.5, E-QH12) and the --out roots (plan point (m))."""

    def test_safe_rmtree_refusals(self) -> None:
        root = self.tmp / "out"
        real = root / "real"
        (real / "sub").mkdir(parents=True)
        (real / "sub" / "f").write_text("keep\n")
        sibling = self.tmp / "sibling"
        sibling.mkdir()
        for path in (sibling, root, real / ".." / ".." / "sibling"):
            with self.assertRaises(HeavyError) as err:
                qor_heavy.safe_rmtree(path, root)
            self.assert_heavy(err, 2, f"refusing to delete {path}: not under {root}")
        self.assertTrue(sibling.is_dir())
        link = root / "link"
        link.symlink_to(real)
        through = link / "sub"
        with self.assertRaises(HeavyError) as err:
            qor_heavy.safe_rmtree(through, root)
        self.assert_heavy(err, 2, f"refusing to delete {through}: {link} is a symlink")
        self.assertEqual((real / "sub" / "f").read_text(), "keep\n")
        with self.assertRaises(HeavyError) as err:
            qor_heavy.safe_rmtree(link, root)
        self.assert_heavy(err, 2, f"refusing to delete {link}: {link} is a symlink")
        self.assertTrue(real.is_dir())
        foreign = Path("/srv/odin3-qor-heavy-test")
        with self.assertRaises(HeavyError) as err:
            qor_heavy.safe_rmtree(foreign / "x", foreign)
        self.assert_heavy(err, 2, f"refusing to delete {foreign / 'x'}: not under {foreign}")
        qor_heavy.safe_rmtree(real / "sub", root)
        self.assertFalse((real / "sub").exists())
        qor_heavy.safe_rmtree(real / "absent", root)  # absent: a no-op
        self.assertTrue(real.is_dir())
        (real / "file").write_text("x\n")  # a regular file is unlinked (Task 5 deletes files)
        qor_heavy.safe_rmtree(real / "file", root)
        self.assertFalse((real / "file").exists())
        self.assertIsNone(qor_heavy._other_device(real, os.stat(root).st_dev))

    def test_safe_rmtree_more(self) -> None:
        """M6g: '..' through a symlink, a symlinked root, and a symlink inside the tree."""
        root = self.tmp / "out"
        real = root / "real"
        (real / "x").mkdir(parents=True)
        (root / "x").mkdir()
        link = root / "link"
        link.symlink_to(real)
        dotted = Path(f"{link}/../x")
        with self.assertRaises(HeavyError) as err:
            qor_heavy.safe_rmtree(dotted, root)
        self.assert_heavy(err, 2, f"refusing to delete {dotted}: not under {root}")
        self.assertTrue((root / "x").is_dir())
        alias = self.tmp / "alias"
        alias.symlink_to(root)
        with self.assertRaises(HeavyError) as err:
            qor_heavy.safe_rmtree(alias / "x", alias)
        self.assert_heavy(err, 2, f"refusing to delete {alias / 'x'}: {alias} is a symlink")
        self.assertTrue((root / "x").is_dir())
        keep = self.tmp / "keep"
        keep.mkdir()
        (keep / "f").write_text("keep\n")
        (real / "x" / "out-link").symlink_to(keep)
        qor_heavy.safe_rmtree(real / "x", root)
        self.assertFalse((real / "x").exists())
        self.assertEqual((keep / "f").read_text(), "keep\n")
        (real / "y").mkdir()
        with mock.patch.object(qor_heavy, "_other_device", return_value=real / "y"), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.safe_rmtree(real, root)
        self.assert_heavy(
            err, 2, f"refusing to delete {real}: {real / 'y'} is on another file system")
        self.assertTrue((real / "y").is_dir())
        self.assertIsNone(qor_heavy._other_device(real, os.stat(root).st_dev))
        gone = str(real / "y" / "gone")
        vanished = FileNotFoundError(2, "No such file or directory", gone)
        with mock.patch.object(qor_heavy, "_other_device", side_effect=vanished), \
                self.assertRaises(HeavyError) as err:
            qor_heavy.safe_rmtree(real, root)
        self.assert_heavy(err, 2, f"refusing to delete {real}: {gone} changed during the check")
        self.assertTrue((real / "y").is_dir())
        with self.assertRaises(FileNotFoundError):
            qor_heavy._other_device(real / "absent", os.stat(root).st_dev)
        self.assertEqual(qor_heavy._other_device(real, -1), real)

    def test_out_dir_roots(self) -> None:
        home = Path("/odin3-qor-heavy-test-home")  # never created; outside /tmp/claude-1000
        work_out = home / "odin3-ws" / "work" / "vtr-qor"
        self.assertEqual(qor_heavy.check_out_dir(work_out, home), work_out)
        tmp_out = Path(os.path.realpath(qor_heavy.HEAVY_DIR)) / "t"
        self.assertEqual(qor_heavy.check_out_dir(Path("/tmp/claude-1000/t"), home), tmp_out)
        with self.assertRaises(HeavyError) as err:
            qor_heavy.check_out_dir(Path("/srv/x"), home)
        self.assert_heavy(
            err, 2, "refusing --out /srv/x: not under ~/odin3-ws/work or /tmp/claude-1000")
        with self.assertRaises(HeavyError):
            qor_heavy.check_out_dir(home / "odin3-ws" / "work", home)  # the root itself


if __name__ == "__main__":
    unittest.main()

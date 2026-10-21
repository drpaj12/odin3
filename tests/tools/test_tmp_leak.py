"""No temporary directory is left in $TMPDIR by the lower-check tools and opt-equiv: a run that
makes its own work directory removes it after a success and after a failure, and keeps it (and
prints its path) only with ``--keep-tmp``. A private $TMPDIR is set per test and counted."""

from __future__ import annotations

import os
import tempfile
import unittest
from collections.abc import Callable, Sequence
from pathlib import Path
from types import ModuleType
from typing import Any
from unittest import mock

import lower_cells
import lower_common as lc
import lower_diff
import lower_equiv
import lower_gen_diff
import lower_sim
import opt_equiv

from . import helpers

ARGS = ["--odin3", "o", "--yosys", "y"]
FAMILIES: list[tuple[str, ModuleType, str, list[str]]] = [
    ("lower-diff", lower_diff, "lower-diff-", [*ARGS, "d.v"]),
    ("lower-sim", lower_sim, "lower-sim-", ["--odin3", "o", "d.v"]),
    ("lower-cells", lower_cells, "lower-cells-", ["--odin3", "o", "d.v"]),
    ("lower-equiv", lower_equiv, "lower-equiv-", [*ARGS, "d.v"]),
    ("opt-equiv", opt_equiv, "opt-equiv-", [*ARGS, "d.v"]),
]


class LeakTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = helpers.scratch(self, "test-tmp-leak-")
        patcher = mock.patch.dict(os.environ, {"TMPDIR": str(self.tmp)})
        patcher.start()
        self.addCleanup(patcher.stop)
        os.environ.pop(lc.WORK_ENV, None)
        os.environ.pop(lc.KEEP_ENV, None)
        tempfile.tempdir = None
        self.addCleanup(setattr, tempfile, "tempdir", None)

    def left(self) -> list[Path]:
        return sorted(self.tmp.iterdir())

    def call(self, module: ModuleType, argv: Sequence[str], execute: Callable[..., Any]
             ) -> tuple[int, str]:
        with mock.patch.object(module, "execute", execute):
            code, out, err = helpers.run_main(module.main, argv)
        return code, out + err

    def result(self, module: ModuleType, code: int) -> Any:
        """What ``execute`` returns for ``code``: opt-equiv returns (code, keep)."""
        return (code, False) if module is opt_equiv else code

    def test_nothing_is_left_after_a_success(self) -> None:
        for name, module, _, argv in FAMILIES:
            with self.subTest(tool=name):
                def run(args: Any, work: Path, module: ModuleType = module) -> Any:
                    (work / "x.txt").write_text("x", encoding="utf-8")
                    return self.result(module, 0)

                self.assertEqual(self.call(module, argv, run)[0], 0)
                self.assertEqual(self.left(), [])

    def test_nothing_is_left_after_failures(self) -> None:
        raises: list[Callable[[], BaseException]] = [
            lambda: lc.CrashError("odin3", "boom"), lambda: lc.ToolError("bad"),
            lambda: OSError("disk"), lambda: ValueError("v")]
        for name, module, _, argv in FAMILIES:
            for make in raises:
                with self.subTest(tool=name, error=repr(make())):
                    def run(args: Any, work: Path, make: Callable[[], BaseException] = make
                            ) -> Any:
                        (work / "x.txt").write_text("x", encoding="utf-8")
                        raise make()

                    self.assertNotEqual(self.call(module, argv, run)[0], 0)
                    self.assertEqual(self.left(), [])
            with self.subTest(tool=name, error="exit 1"):
                def fail(args: Any, work: Path, module: ModuleType = module) -> Any:
                    return self.result(module, 1)

                self.assertEqual(self.call(module, argv, fail)[0], 1)
                self.assertEqual(self.left(), [])

    def test_keep_tmp_keeps_the_directory_and_prints_its_path(self) -> None:
        for name, module, prefix, argv in FAMILIES:
            with self.subTest(tool=name):
                def run(args: Any, work: Path, module: ModuleType = module) -> Any:
                    return self.result(module, 0)

                _, text = self.call(module, ["--keep-tmp", *argv], run)
                kept = self.left()
                self.assertEqual(len(kept), 1, text)
                self.assertTrue(kept[0].name.startswith(prefix))
                self.assertIn(f"work directory kept: {kept[0].resolve()}", text)
                kept[0].rmdir()

    def test_an_explicit_work_directory_is_never_removed(self) -> None:
        for name, module, _, argv in FAMILIES:
            with self.subTest(tool=name):
                work = self.tmp / "mine"
                def run(args: Any, work: Path, module: ModuleType = module) -> Any:
                    return self.result(module, 0)

                self.call(module, ["--work", str(work), *argv], run)
                self.assertEqual(self.left(), [work])
                work.rmdir()

    def test_the_env_var_keeps_the_directory(self) -> None:
        with mock.patch.dict(os.environ, {lc.KEEP_ENV: "1"}):
            self.call(lower_diff, [*ARGS, "d.v"], lambda args, work: 0)
        self.assertEqual(len(self.left()), 1)


class GenDiffLeakTest(unittest.TestCase):
    """lower-gen-diff removes its directory after a clean run and an error, keeps the seed
    folders of a finding (exit 1 or 3: the output names them) and anything with ``--keep-tmp``."""

    def setUp(self) -> None:
        self.tmp = helpers.scratch(self, "test-tmp-leak-gen-")
        patcher = mock.patch.dict(os.environ, {"TMPDIR": str(self.tmp)})
        patcher.start()
        self.addCleanup(patcher.stop)
        os.environ.pop(lc.WORK_ENV, None)
        tempfile.tempdir = None
        self.addCleanup(setattr, tempfile, "tempdir", None)

    def run_gen(self, code: int | None, *extra: str) -> list[Path]:
        def execute(args: Any, work: Path) -> int:
            if code is None:
                raise lc.ToolError("bad")
            return code

        with mock.patch.object(lower_gen_diff, "execute", execute):
            helpers.run_main(lower_gen_diff.main, [*ARGS, "--seeds", "1..1",
                                                   "--parmys-techlibs", "t", *extra])
        found = sorted(self.tmp.iterdir())
        for path in found:
            path.rmdir()
        return found

    def test_clean_and_error_runs_leave_nothing(self) -> None:
        self.assertEqual(self.run_gen(0), [])
        self.assertEqual(self.run_gen(None), [])
        self.assertEqual(self.run_gen(2), [])

    def test_a_finding_or_keep_tmp_keeps_it(self) -> None:
        self.assertEqual(len(self.run_gen(1)), 1)
        self.assertEqual(len(self.run_gen(3)), 1)
        self.assertEqual(len(self.run_gen(0, "--keep-tmp")), 1)


if __name__ == "__main__":
    unittest.main()

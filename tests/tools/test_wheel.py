"""The pip package (Phase 7 Task 11; spec 4, P-4).

wheel_prefers_bundled_lib: with a bundled library and $ODIN3_LIB both present the binding picks
the bundled one and logs which (always runs). test_wheel_installs_and_reads builds the wheel with
scikit-build-core in a temp venv, installs it in a second fresh venv and reads a BLIF through it;
it compiles ABC, so it runs only under CTest `wheel_build` (sweep label, tools/heavy-run) with
ODIN3_WHEEL_TEST=1. $ODIN3_WHEEL_OUT, when set, keeps the wheel there. Build tools are pinned.
"""

from __future__ import annotations

import importlib.util
import logging
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from . import helpers

BINDING = helpers.REPO_ROOT / "plugins" / "python" / "odin3.py"
BLIF = helpers.REPO_ROOT / "tests" / "cli" / "example_plugin.blif"
PINS = ("scikit-build-core==0.11.6", "build==1.2.2.post1", "cffi==1.17.1")
HAVE_CFFI = importlib.util.find_spec("cffi") is not None


def load_binding(directory: Path) -> object:
    """Import the binding copied into ``directory`` as a module named odin3_under_test."""
    spec = importlib.util.spec_from_file_location("odin3_under_test", directory / "odin3.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module  # dataclasses look the module up while it executes
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless(HAVE_CFFI, "cffi missing")
class BundledLibraryTest(unittest.TestCase):
    def test_wheel_prefers_bundled_lib(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            pkg = Path(tmp) / "pkg"
            pkg.mkdir()
            shutil.copy(BINDING, pkg / "odin3.py")
            (pkg / "libodin3.so").write_bytes(b"")
            other = Path(tmp) / "libother.so"
            other.write_bytes(b"")
            mod = load_binding(pkg)
            old = os.environ.get("ODIN3_LIB")
            os.environ["ODIN3_LIB"] = str(other)
            try:
                with self.assertLogs("odin3", logging.INFO) as logs:
                    found = mod.find_library()  # type: ignore[attr-defined]
                self.assertEqual(found, (pkg / "libodin3.so").resolve())
                self.assertIn("bundled", logs.output[0])
                (pkg / "libodin3.so").unlink()
                self.assertEqual(mod.find_library(), other)  # type: ignore[attr-defined]
            finally:
                if old is None:
                    del os.environ["ODIN3_LIB"]
                else:
                    os.environ["ODIN3_LIB"] = old


def run(argv: list[str], log: Path, env: dict[str, str] | None = None) -> str:
    """Run argv with output in ``log``; returns stdout text only for quiet commands."""
    with log.open("w") as handle:
        proc = subprocess.run(argv, stdout=handle, stderr=subprocess.STDOUT, env=env,
                              check=False)
    if proc.returncode != 0:
        tail = "\n".join(log.read_text().splitlines()[-15:])
        raise AssertionError(f"{argv[:4]} failed ({proc.returncode}); {log}:\n{tail}")
    return log.read_text()


@unittest.skipUnless(os.environ.get("ODIN3_WHEEL_TEST") == "1",
                     "builds the wheel (compiles ABC): run CTest wheel_build")
class WheelTest(unittest.TestCase):
    def test_wheel_installs_and_reads(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            keep = os.environ.get("ODIN3_WHEEL_OUT")
            out = Path(keep) if keep else work / "dist"
            out.mkdir(parents=True, exist_ok=True)
            build_venv = work / "buildvenv"
            run([sys.executable, "-m", "venv", str(build_venv)], work / "venv1.log")
            py = str(build_venv / "bin" / "python")
            run([py, "-m", "pip", "install", "--quiet", *PINS], work / "pip1.log")
            env = dict(os.environ, CMAKE_BUILD_PARALLEL_LEVEL="4")
            run([py, "-m", "build", "--wheel", "--no-isolation", "--outdir", str(out),
                 str(helpers.REPO_ROOT)], work / "build.log", env)
            wheels = sorted(out.glob("odin3-1.1.1-*.whl"))
            self.assertEqual(len(wheels), 1, wheels)
            install_venv = work / "installvenv"
            run([sys.executable, "-m", "venv", str(install_venv)], work / "venv2.log")
            ipy = str(install_venv / "bin" / "python")
            run([ipy, "-m", "pip", "install", "--quiet", str(wheels[0])], work / "pip2.log")
            code = ("import odin3, sys; d = odin3.Design.read_blif(sys.argv[1]); "
                    "print('MODULE', [m.name for m in d.modules()][0]); "
                    "print('LIB', odin3.find_library())")
            env = {k: v for k, v in os.environ.items() if k != "ODIN3_LIB"}
            text = run([ipy, "-c", code, str(BLIF)], work / "read.log", env)
            self.assertIn("MODULE ", text)
            self.assertIn("site-packages/odin3/libodin3.so", text)
            # The library alone registers the in-process ABC engine: abc runs from Python.
            adder = work / "add.v"
            adder.write_text("module top(input [3:0] a, b, output [4:0] y); assign y = a + b; "
                             "endmodule\n")
            abc_code = ("import odin3, sys; d = odin3.Design(); "
                        "d.run_script('read_verilog ' + sys.argv[1] + '; proc; opt; lower; "
                        "abc --lut 4'); print('ABC OK')")
            self.assertIn("ABC OK", run([ipy, "-c", abc_code, str(adder)], work / "abc.log", env))
            # The flow tools ship in the wheel: odin3-flow runs from outside the repo.
            flow = install_venv / "bin" / "odin3-flow"
            self.assertTrue(flow.is_file(), flow)
            run([str(flow), "--target", "lpm", "--no-vpr", "--out", str(work / "flow-out"),
                 str(adder)], work / "flow.log", env)
            self.assertTrue((install_venv / "bin" / "odin3").is_file())
            cli = install_venv / "lib"
            found = list(cli.glob("python*/site-packages/odin3/bin/odin3"))
            self.assertEqual(len(found), 1)
            ver = run([str(found[0]), "--version"], work / "cli.log")
            self.assertIn("odin3 1.1.1", ver)


if __name__ == "__main__":
    unittest.main()

"""Release packaging (Phase 7 Task 10; spec 4, P-1 to P-3): the version, the install tree and the
source tarball. The CLI and install tests need ODIN3_CLI (the built odin3; its directory is the
CMake build tree), set by CTest python_tools; they skip without it."""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from . import helpers

CLI = os.environ.get("ODIN3_CLI", "")
BUILD_DIR = Path(CLI).parent if CLI else None
MAKE_TARBALL = helpers.REPO_ROOT / "tools" / "release" / "make-tarball"


def run(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, capture_output=True, text=True, check=False)


class VersionTest(unittest.TestCase):
    @unittest.skipUnless(CLI, "ODIN3_CLI not set (run CTest python_tools)")
    def test_version_is_1_1_1(self) -> None:
        out = run([CLI, "--version"])
        self.assertEqual(out.returncode, 0, out.stderr)
        self.assertRegex(out.stdout, r"^odin3 1\.1\.1 \(ABI \d+\)")

    def test_header_version_is_1_1_1(self) -> None:
        # odin3_version_string() is built from these three macros (src/api/version.c).
        header = (helpers.REPO_ROOT / "include" / "odin3" / "odin3.h").read_text()
        parts = [re.search(rf"#define ODIN3_VERSION_{name} (\d+)", header)
                 for name in ("MAJOR", "MINOR", "PATCH")]
        self.assertEqual([m.group(1) if m else None for m in parts], ["1", "1", "1"])

    def test_pyproject_version_is_1_1_1(self) -> None:
        text = (helpers.REPO_ROOT / "pyproject.toml").read_text()
        self.assertRegex(text, r'(?m)^version = "1\.1\.1"$')


@unittest.skipUnless(CLI, "ODIN3_CLI not set (run CTest python_tools)")
class InstallTreeTest(unittest.TestCase):
    def test_install_tree(self) -> None:
        assert BUILD_DIR is not None
        with tempfile.TemporaryDirectory() as tmp:
            prefix = Path(tmp) / "prefix"
            out = run(["cmake", "--install", str(BUILD_DIR), "--prefix", str(prefix)])
            self.assertEqual(out.returncode, 0, out.stderr[-500:])
            for rel in ("bin/odin3", "include/odin3/odin3.h",
                        "share/odin3/lib/vtr.o3lib", "share/odin3/lib/abc/vtr.abc",
                        "share/odin3/tools/flow/odin3_flow.py",
                        "share/odin3/tools/flow/odin3-flow",
                        "share/odin3/tools/flow/lpm.script",
                        "share/odin3/tools/vtr-qor/qor_legs.py"):
                self.assertTrue((prefix / rel).is_file(), rel)
            self.assertTrue(list((prefix / "lib").glob("libodin3.so*")), "lib/libodin3.so")
            self.assertTrue(list((prefix / "share/odin3/lib").glob("*.o3lib")))
            # The installed CLI runs without the build tree (RPATH to ../lib).
            ver = run([str(prefix / "bin" / "odin3"), "--version"])
            self.assertEqual(ver.returncode, 0, ver.stderr)
            self.assertIn("odin3 1.1.1", ver.stdout)


class TarballTest(unittest.TestCase):
    def test_tarball_has_submodules(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = run([str(MAKE_TARBALL), "--out", tmp])
            self.assertEqual(out.returncode, 0, out.stderr[-500:])
            tarball = Path(tmp) / "odin3-1.1.1.tar.gz"
            sums = Path(tmp) / "SHA256SUMS"
            self.assertTrue(tarball.is_file())
            self.assertTrue(sums.is_file())
            digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
            self.assertIn(f"{digest}  {tarball.name}", sums.read_text())
            with tarfile.open(tarball) as tar:
                names = tar.getnames()
            for prefix in ("third_party/abc/", "third_party/unity/"):
                self.assertTrue(
                    any(n.startswith(f"odin3-1.1.1/{prefix}") and n.endswith(".c")
                        for n in names), prefix)
            self.assertIn("odin3-1.1.1/CMakeLists.txt", names)
            with tarfile.open(tarball) as tar:
                for sub in ("abc", "unity"):
                    stamp = tar.extractfile(f"odin3-1.1.1/third_party/{sub}/.odin3-commit")
                    assert stamp is not None
                    sha = run(["git", "-C", str(helpers.REPO_ROOT / "third_party" / sub),
                               "rev-parse", "HEAD"]).stdout.strip()
                    self.assertEqual(stamp.read().decode().strip(), sha, sub)
            self.assertFalse(any("/.git/" in n or "/build/" in n for n in names))

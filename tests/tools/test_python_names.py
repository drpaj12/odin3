"""V1.2.0 Task 6: `odin3.read_name_map` agrees with the C parse of `names`.

The maps are the ones tests/tools/test_names_v2.py writes (two micros, three writers, version 2,
and a version 1 map). For each map the Python reader gives the same line counts as `names
--check`, and for a sample of objects the same records (`NameMap.provenance`), the same chain
(`NameMap.chain`) and the same files and runs as `names --lookup` and `--chain`, line for line.
Run through CTest `python_tools_names` (ODIN3_CLI set); skipped without cffi or ODIN3_CLI, unless
ODIN3_REQUIRE_CFFI=1.
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from .test_names_v2 import CASES, ODIN3, names, write_map

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False
REQUIRE_CFFI = os.environ.get("ODIN3_REQUIRE_CFFI") == "1"

if TYPE_CHECKING or HAVE_CFFI:
    from odin3 import NameMap, read_name_map

SAMPLE = 30


def sample(nm: NameMap) -> list[str]:
    """Names of objects that match exactly one line, spread over the map (first and last in)."""
    step = max(1, len(nm.objects) // SAMPLE)
    picked = nm.objects[::step] + nm.objects[-1:]
    found: list[str] = []
    for obj in picked:
        if len(nm.find(obj.name)) == 1 and obj.name not in found:
            found.append(obj.name)
    return found


def numbers(lines: list[str], letter: str) -> list[int]:
    return [int(line.split("\t")[1][1:]) for line in lines if line.startswith(letter + "\t")]


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (run CTest python_tools_names)")
class ReadNameMapTest(unittest.TestCase):
    work: Path

    @classmethod
    def setUpClass(cls) -> None:
        if not HAVE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        base = Path("/tmp/claude-1000")
        cls.work = Path(tempfile.mkdtemp(prefix="names-py-", dir=base if base.is_dir() else None))

    def test_counts_agree_with_check(self) -> None:
        for micro, writer in CASES:
            path = write_map(self.work, micro, writer, 2)
            nm = read_name_map(path)
            _code, out, _err = names(f"--map {path} --check")
            first, second = out.splitlines()
            self.assertEqual(nm.version, 2)
            self.assertEqual(nm.ast, "refs")
            self.assertEqual(nm.ignored, 0)
            self.assertEqual(len([o for o in nm.objects if o.type == "O"]),
                             int(first.split()[0].split("=")[1]))
            counts = dict(item.split("=") for item in second.split())
            self.assertEqual(len(nm.records), int(counts["records"]))
            self.assertEqual(len(nm.constructs), int(counts["constructs"]))
            self.assertEqual(len(nm.files), int(counts["files"]))
            self.assertEqual(len(nm.runs), int(counts["runs"]))
            self.assertEqual(len(nm.objects), int(counts["objects"]))

    def test_records_and_chain_agree_with_lookup(self) -> None:
        total = 0
        for micro, writer in CASES:
            path = write_map(self.work, micro, writer, 2)
            nm = read_name_map(path)
            for name in sample(nm):
                (obj,) = nm.find(name)
                code, out, err = names(f"--map {path} --lookup {name}")
                self.assertEqual(code, 0, err)
                lines = [line.strip(" ") for line in out.splitlines()]
                records = [r.raw for r in nm.provenance(obj)]
                chain = [c.raw for c in nm.chain(obj)]
                self.assertEqual(lines[0], obj.raw)
                self.assertEqual([x for x in lines if x.startswith("R\t")], records, name)
                self.assertEqual([x for x in lines if x.startswith("C\t")], chain, name)
                want_files = {int(o.split(":")[0][1:]) for r in nm.provenance(obj)
                              for o in r.origins if o[0] == "f"} | {c.file for c in nm.chain(obj)
                                                                   if c.file}
                self.assertEqual(sorted(want_files), numbers(lines, "F"), name)
                want_runs = {int(r.step.split(".")[0][1:]) for r in nm.provenance(obj)}
                self.assertEqual(sorted(want_runs), numbers(lines, "T"), name)
                code, out, err = names(f"--map {path} --chain {name}")
                self.assertEqual(code, 0, err)
                self.assertEqual([x for x in out.splitlines() if x.startswith("C\t")], chain)
                total += 1
        self.assertGreater(total, 40)

    def test_the_chain_reaches_the_module(self) -> None:
        path = write_map(self.work, "ch_intrinsics", "blif", 2)
        nm = read_name_map(path)
        obj = next(o for o in nm.objects if o.records)
        chain = nm.chain(obj)
        self.assertTrue(chain)
        self.assertIn("MODULE", [c.kind for c in chain])
        self.assertEqual(nm.files[chain[0].file - 1], "tests/micro/verilog/full/ch_intrinsics.v")

    def test_version_1_map(self) -> None:
        path = write_map(self.work, "ansiportlist_2", "blif", 1)
        nm = read_name_map(path)
        _code, out, _err = names(f"--map {path} --check")
        self.assertEqual(nm.version, 1)
        self.assertEqual(len(nm.objects), int(out.split()[0].split("=")[1]))
        self.assertEqual(nm.records, [])
        self.assertEqual(nm.provenance(nm.objects[0]), [])
        self.assertEqual(nm.chain(nm.objects[0]), [])

    def test_unknown_types_are_skipped_and_atoms_kept(self) -> None:
        path = write_map(self.work, "ansiportlist_2", "blif", 2)
        extra = self.work / "extra.names"
        extra.write_text(path.read_text() + "A\tatom_0\tr1\t-\nZ\tx\nZ\ty\nR\tr9\n")
        with self.assertLogs("odin3", level="WARNING") as logs:
            nm = read_name_map(extra)
        self.assertEqual(nm.atoms, [("atom_0", "r1", "-")])
        self.assertEqual(nm.ignored, 3)  # two Z lines and the short R line
        self.assertEqual(len([m for m in logs.output if "'Z' is unknown" in m]), 1)

    def test_bad_headers_raise(self) -> None:
        bad = self.work / "bad.names"
        for text in ("", "hello\n", "# odin3-names 3 hash=sha256 width=10\n",
                     "# odin3-names 2 hash=sha256 width=10\n"):
            bad.write_text(text)
            with self.assertRaises(ValueError):
                read_name_map(bad)

    def test_escapes(self) -> None:
        path = self.work / "esc.names"
        path.write_text("# odin3-names 1 hash=sha256 width=10\n"
                        "%23m%09x\tnet\th\tf%2Cg%7Ch\th%25\tf%5C\t7\n")
        obj = read_name_map(path).objects[0]
        got = (obj.module, obj.full, obj.hier, obj.file)
        self.assertEqual(got, ("#m\tx", "f,g|h", "h%", "f\\"))


if __name__ == "__main__":
    unittest.main()

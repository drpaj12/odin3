"""V1.2.0 Task 6: `odin3.read_name_map` agrees with the C parse of `names`.

The maps are the ones tests/tools/test_names_v2.py writes (two micros, three writers, version 2,
and a version 1 map). For each map the Python reader gives the same line counts as `names
--check`, and for a sample of objects the same records (`NameMap.provenance`), the same chain
(`NameMap.chain`) and the same files and runs as `names --lookup` and `--chain`, line for line.
Run through CTest `python_names` (ODIN3_CLI set); skipped without cffi or ODIN3_CLI, unless
ODIN3_REQUIRE_CFFI=1.
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

from .helpers import REPO_ROOT
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
MICRO_SET = "tests/micro/verilog/micro"
SKIP_BIG_BYTES = 40000


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
@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (run CTest python_names)")
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

    def agree(self, path: Path, nm: NameMap, name: str) -> None:
        """names --lookup and --chain of one object give the records and chain nm gives."""
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

    def test_records_and_chain_agree_with_lookup(self) -> None:
        total = 0
        for micro, writer in CASES:
            path = write_map(self.work, micro, writer, 2)
            nm = read_name_map(path)
            for name in sample(nm):
                self.agree(path, nm, name)
                total += 1
        self.assertGreater(total, 40)

    def test_micro_set(self) -> None:
        """Every micro design (the small ones, as the Task 5 map test): the counts of the Python
        reader equal `names --check`, and six objects per map agree record for record."""
        maps = 0
        for design in sorted((REPO_ROOT / MICRO_SET).glob("*.v")):
            if design.stat().st_size > SKIP_BIG_BYTES:
                continue
            try:
                path = write_map(self.work, design.stem, "blif", 2, MICRO_SET)
            except AssertionError:
                continue  # a micro the flow refuses has no map
            nm = read_name_map(path)
            _code, out, _err = names(f"--map {path} --check")
            counts = dict(item.split("=") for item in out.splitlines()[1].split())
            self.assertEqual(
                (len(nm.records), len(nm.constructs), len(nm.files), len(nm.runs),
                 len(nm.objects)),
                tuple(int(counts[key]) for key in
                      ("records", "constructs", "files", "runs", "objects")), design.name)
            for name in sample(nm)[:6]:
                self.agree(path, nm, name)
            maps += 1
        self.assertGreater(maps, 20)

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

    def write(self, name: str, text: str | bytes) -> Path:
        path = self.work / name
        path.write_bytes(text.encode("utf-8") if isinstance(text, str) else text)
        return path

    def test_escapes(self) -> None:
        """Version 1 knows five escapes; %2C and %7C are version 2's, as in the C parse."""
        header = "# odin3-names 1 hash=sha256 width=10\n"
        path = self.write("esc.names", header + "%23m%09x\tnet\th\tf%25g\th%25\tf%5C\t7\n")
        obj = read_name_map(path).objects[0]
        self.assertEqual((obj.module, obj.full, obj.hier, obj.file), ("#m\tx", "f%g", "h%", "f\\"))
        path = self.write("esc1.names", header + "m\tnet\th\tf%2Cg\th\tf\t7\n")
        with self.assertLogs("odin3", level="WARNING"):
            self.assertEqual(read_name_map(path).ignored, 1)
        two = "# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs\n"
        row = "O\tm\tnet\th\tf%2Cg%7Ch\th\tf\t7\t\t\n"
        obj = read_name_map(self.write("esc2.names", two + row)).objects[0]
        self.assertEqual(obj.full, "f,g|h")

    def test_decimals_are_ascii_and_capped(self) -> None:
        two = "# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs\n"
        rows = {"7": 7, "4294967295": 4294967295, "0000000012": 12, "4294967296": None,
                "\u0663": None, "+5": None, "": None, "00000000012": None}
        for text, want in rows.items():
            row = f"O\tm\tnet\th\tf\th\tf\t{text}\t\t\n"
            with self.subTest(text=text):
                path = self.write("dec.names", two + row)
                if want is None:
                    with self.assertLogs("odin3", level="WARNING"):
                        nm = read_name_map(path)
                else:
                    nm = read_name_map(path)
                self.assertEqual(len(nm.objects), 0 if want is None else 1)
                self.assertEqual(nm.ignored, 1 if want is None else 0)
                if want is not None:
                    self.assertEqual(nm.objects[0].line, want)
        # a reference number goes through the same parse
        row = "R\t" + "\t".join(["r\u0661", "S", "stmt", "origin", "t1.1", "", "", "", ""]) + "\n"
        with self.assertLogs("odin3", level="WARNING"):
            self.assertEqual(read_name_map(self.write("ref.names", two + row)).ignored, 1)

    def test_lines_the_parse_ignores(self) -> None:
        one = "# odin3-names 1 hash=sha256 width=10\n"
        two = "# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs\n"
        good1 = "m\tnet\th\tf\th\tf\t7"
        good2 = "O\tm\tnet\th\tf\th\tf\t7\t\t"
        cases: dict[str, str | bytes] = {
            "v1_cr": one + good1 + "\r\n",
            "v1_kind": one + good1.replace("net", "reg") + "\n",
            "v1_utf8": one.encode() + b"m\xff\tnet\th\tf\th\tf\t7\n",
            "v2_kind": two + good2.replace("net", "reg") + "\n",
            "v2_cr": two + good2 + "\r\n",
            "v2_utf8": two.encode() + b"O\tm\xff\tnet\th\tf\th\tf\t7\t\t\n",
            "long": two + "O\t" + "x" * (1 << 20) + "\n",
        }
        for label, text in cases.items():
            with self.subTest(label):
                path = self.write(f"{label}.names", text)
                with self.assertLogs("odin3", level="WARNING") as logs:
                    nm = read_name_map(path)
                self.assertEqual((len(nm.objects), nm.ignored), (0, 1))
                self.assertIn(f"{path}:2:", logs.output[0])
                code, _out, _err = names(f"--map {path} --check")
                self.assertNotEqual(code, 0)  # the C parse ignores it too
        nm = read_name_map(self.write("ok1.names", one + good1 + "\n"))
        self.assertEqual((len(nm.objects), nm.ignored), (1, 0))

    def test_only_twelve_fields_are_read(self) -> None:
        two = "# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs\n"
        row = "F\tf1\tt%zz\n"  # a bad escape in a field that is read
        with self.assertLogs("odin3", level="WARNING"):
            nm = read_name_map(self.write("many.names", two + row))
        self.assertEqual((nm.files, nm.ignored), ([], 1))
        row = "F\tf1\tt.v" + "\tx" * 10 + "\t%zz\n"  # a bad escape in the 13th field only
        nm = read_name_map(self.write("many2.names", two + row))
        self.assertEqual((nm.files, nm.ignored), (["t.v"], 0))

    def test_find_uses_an_index_that_follows_the_objects(self) -> None:
        path = write_map(self.work, "ansiportlist_2", "blif", 2)
        nm = read_name_map(path)
        obj = nm.objects[0]
        self.assertIn(obj, nm.find(obj.name))
        self.assertEqual(nm.find("no such name"), [])
        nm.objects.append(obj)
        self.assertEqual(nm.find(obj.name).count(obj), 2)

    def test_dangling_parent_agrees_with_the_c_walk(self) -> None:
        """A record whose parent does not resolve: `names --lookup` skips it, and so does the
        reader (provenance() and chain() give the same lines)."""
        path = write_map(self.work, "ch_intrinsics", "blif", 2)
        nm = read_name_map(path)
        name = next(n for n in sample(nm)
                    if len(nm.provenance(nm.find(n)[0])) > 1)
        first = nm.provenance(nm.find(name)[0])[0].raw
        lines = path.read_text().split("\n")
        index = lines.index(first)
        fields = lines[index].split("\t")
        fields[6] = "r99999,,r1" if int(fields[1][1:]) > 1 else "r99999"
        lines[index] = "\t".join(fields)
        planted = self.write("dangling.names", "\n".join(lines))
        code, _out, _err = names(f"--map {planted} --check")
        self.assertNotEqual(code, 0)  # --check counts the references that do not resolve
        planted_nm = read_name_map(planted)
        self.assertEqual(planted_nm.ignored, 0)
        self.agree(planted, planted_nm, name)
        self.assertLess(len(planted_nm.provenance(planted_nm.find(name)[0])),
                        len(nm.provenance(nm.find(name)[0])))


if __name__ == "__main__":
    unittest.main()

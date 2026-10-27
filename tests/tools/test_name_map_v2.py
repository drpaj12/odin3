"""V1.2.0 Task 7: tools/flow/name_map_v2.py, the stdlib copy of `odin3.read_name_map`.

The flow tools cannot import the odin3 package (it needs cffi), so the parse is copied; this test
pins the two together. For every small micro map (version 2, from the Task 5 writers) and a set of
hand-made malformed lines, the stdlib parser gives the same objects, records, chains, files, runs,
atoms, ignored count and warnings as `odin3.read_name_map`, and it ignores the lines the C
`names --check` ignores. `odin3.py` is loaded by path with `cffi` stubbed when it is not installed:
`read_name_map` reads a file and needs no library. Maps come from the odin3 CLI (ODIN3_CLI, set by
CTest python_tools); the tests that need one skip without it. The synthetic-map tests need nothing.
"""

from __future__ import annotations

import dataclasses
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType
from unittest import mock

import name_map_v2 as nm2

from .helpers import REPO_ROOT
from .test_names_v2 import CASES, ODIN3, names, write_map

MICRO_SET = "tests/micro/verilog/micro"
SKIP_BIG_BYTES = 40000
TWO = "# odin3-names 2 hash=sha256 width=10 prov=1 ast=refs\n"
ONE = "# odin3-names 1 hash=sha256 width=10\n"
GOOD2 = "O\tm\tnet\th\tf\th\tf\t7\t\t"
GOOD1 = "m\tnet\th\tf\th\tf\t7"


def load_odin3() -> ModuleType:
    """plugins/python/odin3.py by path; cffi is stubbed when missing (read_name_map needs no
    library)."""
    path = REPO_ROOT / "plugins" / "python" / "odin3.py"
    stub = {} if "cffi" in sys.modules or importlib.util.find_spec("cffi") else {
        "cffi": mock.MagicMock()}
    with mock.patch.dict(sys.modules, stub):
        spec = importlib.util.spec_from_file_location("odin3_under_nm2", path)
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module  # dataclasses look the module up by name
        spec.loader.exec_module(module)
    return module


def plain(nm: object) -> dict[str, object]:
    """The parse of a NameMap as plain data: the same view for both implementations (the private
    index and the flow's own additions are not part of the shared parse)."""
    data = dataclasses.asdict(nm)  # type: ignore[call-overload]
    for key in [k for k in data if k.startswith("_")]:
        del data[key]
    return dict(data)


def texts(log: mock.MagicMock) -> list[tuple[str, ...]]:
    """The calls of a mocked logger as strings (exceptions do not compare equal)."""
    return [tuple(str(a) for a in call.args) for call in log.call_args_list]


class Agreement:
    """Mixin: compare the two parsers on a file."""

    work: Path
    odin3: ModuleType

    def agree(self, path: Path) -> tuple[nm2.NameMap, int]:
        case: unittest.TestCase = self  # type: ignore[assignment]
        with mock.patch.object(nm2.LOG, "warning") as stdlib_log:
            ours = nm2.read_name_map(path)
        with mock.patch.object(self.odin3.LOG, "warning") as odin3_log:
            theirs = self.odin3.read_name_map(path)
        case.assertEqual(plain(ours), plain(theirs), str(path))
        case.assertEqual(texts(stdlib_log), texts(odin3_log), str(path))
        return ours, stdlib_log.call_count


class SyntheticMaps(Agreement, unittest.TestCase):
    """Hand-made maps: no odin3 CLI needed except in the `--check` comparison below."""

    @classmethod
    def setUpClass(cls) -> None:
        base = Path("/tmp/claude-1000")
        cls.work = Path(tempfile.mkdtemp(prefix="flowprov-nm2-",
                                         dir=base if base.is_dir() else None))
        cls.odin3 = load_odin3()

    def write(self, name: str, text: str | bytes) -> Path:
        path = self.work / name
        path.write_bytes(text.encode("utf-8") if isinstance(text, str) else text)
        return path

    def test_every_line_type_and_unknown_type(self) -> None:
        text = (TWO + "F\tf1\ttests/a.v\nT\tt1\tread_verilog\t-\tinfo\n"
                "C\tc1\tMODULE\tmodule\t\tf1\t1.1-9.10\tm\n"
                "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:3.5-3.20\tc1\t\n"
                "O\tm\tnet\th1\tm/x@a.v:3\tm\ta.v\t3\tr1\tstmt\n"
                "N\tm\tnode\tclk\tr1\tstmt\nA\tatom_0\tr1\t-\nZ\tx\nZ\ty\n")
        ours, warnings = self.agree(self.write("types.names", text))
        self.assertEqual((len(ours.objects), len(ours.records), len(ours.constructs),
                          ours.atoms, ours.ignored), (2, 1, 1, [("atom_0", "r1", "-")], 2))
        self.assertEqual(warnings, 1)  # one warning per unknown type

    def test_malformed_lines_are_ignored_as_the_c_parse_does(self) -> None:
        cases: dict[str, str | bytes] = {
            "v1_cr": ONE + GOOD1 + "\r\n",
            "v1_kind": ONE + GOOD1.replace("net", "reg") + "\n",
            "v1_utf8": ONE.encode() + b"m\xff\tnet\th\tf\th\tf\t7\n",
            "v1_fields": ONE + "m\tnet\th\n",
            "v2_kind": TWO + GOOD2.replace("net", "reg") + "\n",
            "v2_cr": TWO + GOOD2 + "\r\n",
            "v2_utf8": TWO.encode() + b"O\tm\xff\tnet\th\tf\th\tf\t7\t\t\n",
            "v2_short": TWO + "R\tr9\n",
            "v2_no_tab": TWO + "Ox\n",
            "v2_escape": TWO + "F\tf1\tt%zz\n",
            "v2_ref": TWO + "R\tr١\tS\tstmt\torigin\tt1.1\t\t\t\n",
            "v2_decimal": TWO + GOOD2.replace("\t7\t", "\t4294967296\t") + "\n",
            "long": TWO + "O\t" + "x" * (1 << 20) + "\n",
        }
        for label, text in cases.items():
            with self.subTest(label):
                path = self.write(f"{label}.names", text)
                ours, warnings = self.agree(path)
                self.assertEqual((len(ours.objects), ours.ignored, warnings), (0, 1, 1))

    def test_good_lines_are_kept(self) -> None:
        for text in (ONE + GOOD1 + "\n", TWO + GOOD2 + "\n", TWO + GOOD2 + "\n\n# comment\n"):
            ours, warnings = self.agree(self.write("good.names", text))
            self.assertEqual((len(ours.objects), ours.ignored, warnings), (1, 0, 0))

    def test_only_twelve_fields_are_read(self) -> None:
        ours, _ = self.agree(self.write("many.names", TWO + "F\tf1\tt.v" + "\tx" * 10 + "\t%zz\n"))
        self.assertEqual((ours.files, ours.ignored), (["t.v"], 0))

    def test_bad_headers_raise_in_both(self) -> None:
        for text in ("", "hello\n", "# odin3-names 3 hash=sha256 width=10\n",
                     "# odin3-names 2 hash=sha256 width=10\n",
                     "# odin3-names 1 hash=sha256 width=10 prov=1 ast=refs\n"):
            path = self.write("bad.names", text)
            with self.subTest(text=text), self.assertRaises(ValueError):
                nm2.read_name_map(path)
            with self.subTest(odin3=text), self.assertRaises(ValueError):
                self.odin3.read_name_map(path)

    def test_escapes_in_both_versions(self) -> None:
        text = ONE + "%23m%09x\tnet\th\tf%25g\th%25\tf%5C\t7\n"
        ours, _ = self.agree(self.write("esc1.names", text))
        obj = ours.objects[0]
        self.assertEqual((obj.module, obj.full, obj.hier, obj.file), ("#m\tx", "f%g", "h%", "f\\"))
        ours, warnings = self.agree(self.write("esc1c.names", ONE + "m\tnet\th\tf%2Cg\th\tf\t7\n"))
        self.assertEqual((ours.ignored, warnings), (1, 1))  # %2C is a version 2 escape
        row = "O\tm\tnet\th\tf%2Cg%7Ch\th\tf\t7\t\t\n"
        ours, _ = self.agree(self.write("esc2.names", TWO + row))
        self.assertEqual(ours.objects[0].full, "f,g|h")


class Helpers(unittest.TestCase):
    """The flow's own additions on a small map."""

    TEXT = (TWO + "F\tf1\ta.v\nF\tf2\tb.v\nT\tt1\tread_verilog\t-\tinfo\n"
            "C\tc1\tMODULE\tmodule\t\tf1\t1.1-9.10\tm\n"
            "C\tc2\tASSIGN\tstmt\tc1\tf1\t3.5-3.20\t\n"
            "R\tr1\tS\tstmt\torigin\tt1.1\t\tf1:3.5-3.20\tc2\t\n"
            "R\tr2\tI\tstmt\torigin\tt1.1\t\t\t\t\n"
            "R\tr3\tD\tlut\tcoarsen:gate\tt1.1\tr2,r1\t\t\t\n"
            "O\tm\tnet\th1\tm/x@a.v:3\tm\ta.v\t3\tr3\tlut\n"
            "O\tn\tnet\th1\tn/x@b.v:4\tn\tb.v\t4\tr1\tstmt\n"
            "N\tm\tnode\th1\tr1\tstmt\n")

    def load(self) -> nm2.NameMap:
        tmp = Path(tempfile.mkdtemp(prefix="flowprov-nm2h-", dir="/tmp/claude-1000"))
        path = tmp / "h.names"
        path.write_text(self.TEXT)
        return nm2.read_name_map(path)

    def test_atom_prefers_net_and_module(self) -> None:
        nm = self.load()
        first = nm.atom("h1")
        assert first is not None
        self.assertEqual((first.kind, first.module), ("net", "m"))
        other = nm.atom("h1", "n")
        assert other is not None
        self.assertEqual(other.module, "n")
        self.assertIsNone(nm.atom("nothing"))

    def test_leaf_skips_derived_and_unlocated_records(self) -> None:
        nm = self.load()
        obj = nm.atom("h1")
        assert obj is not None
        found = nm.leaf(obj)
        assert found is not None
        record, span = found
        self.assertEqual(record.number, 1)  # r3 (D) and r2 (I without a location) are skipped
        self.assertEqual(span, nm2.SourceSpan("a.v", 3, 5, 3, 20))
        self.assertEqual((span.text, span.short), ("a.v:3.5-3.20", "a.v:3"))

    def test_unreadable_origin_gives_none(self) -> None:
        nm = self.load()
        self.assertIsNone(nm.location(nm.records[1]))
        bad = dataclasses.replace(nm.records[0], origins=("f9:1.1-1.2", "fx", "f1:3.5"))
        self.assertIsNone(nm.location(bad))


@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools sets it)")
class MicroMaps(Agreement, unittest.TestCase):
    """The maps of the micro set and the four Task 6 cases, written by the odin3 CLI."""

    @classmethod
    def setUpClass(cls) -> None:
        base = Path("/tmp/claude-1000")
        cls.work = Path(tempfile.mkdtemp(prefix="flowprov-nm2m-",
                                         dir=base if base.is_dir() else None))
        cls.odin3 = load_odin3()

    def test_task6_cases(self) -> None:
        for micro, writer in CASES:
            path = write_map(self.work, micro, writer, 2)
            ours, warnings = self.agree(path)
            self.assertEqual((ours.ignored, warnings), (0, 0))
            self.assertTrue(ours.records and ours.constructs and ours.files and ours.runs)
            _code, out, _err = names(f"--map {path} --check")
            counts = dict(item.split("=") for item in out.splitlines()[1].split())
            self.assertEqual(
                (len(ours.records), len(ours.constructs), len(ours.files), len(ours.runs),
                 len(ours.objects)),
                tuple(int(counts[k]) for k in ("records", "constructs", "files", "runs",
                                               "objects")))

    def test_version_1_map(self) -> None:
        path = write_map(self.work, "ansiportlist_2", "blif", 1)
        ours, _ = self.agree(path)
        self.assertEqual((ours.version, ours.records), (1, []))

    def test_micro_set(self) -> None:
        maps = 0
        for design in sorted((REPO_ROOT / MICRO_SET).glob("*.v")):
            if design.stat().st_size > SKIP_BIG_BYTES:
                continue
            try:
                path = write_map(self.work, design.stem, "blif", 2, MICRO_SET)
            except AssertionError:
                continue  # a micro the flow refuses has no map
            ours, _ = self.agree(path)
            for obj in ours.objects[:: max(1, len(ours.objects) // 6)]:
                theirs = self.odin3.read_name_map(path)
                (mine,) = [o for o in theirs.find(obj.name) if o.raw == obj.raw][:1]
                self.assertEqual([r.raw for r in ours.provenance(obj)],
                                 [r.raw for r in theirs.provenance(mine)])
                self.assertEqual([c.raw for c in ours.chain(obj)],
                                 [c.raw for c in theirs.chain(mine)])
            maps += 1
        self.assertGreater(maps, 20)


if __name__ == "__main__":
    unittest.main()

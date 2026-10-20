"""Phase 1 exit test: a Python plugin walks the IR through the C ABI (plugins/python).

Every committed BLIF fixture is read through the cffi binding and walked from Python; the
per-module counts and the cell-type histogram must equal what the C `stats` pass logs for the
same design (captured through the ABI's log sink, not re-implemented). Run through CTest
`python_walk`, which sets ODIN3_LIB (and preloads the sanitizer runtime for a Debug library).
Skipped where cffi is missing, unless ODIN3_REQUIRE_CFFI=1 (CI sets it: there a missing cffi
fails the test instead of hiding it), and where ODIN3_LIB is unset (`python_tools` runs this
directory without a library and without the sanitizer runtime). CTest `python_walk` reports
either skip as Skipped, never as Passed.

ProcWalkTest (2D Task 6) walks Task 3's $proc node P1 through the ABI. No front end makes one
yet, so it loads the test-only odin3_procfix library ($ODIN3_PROC_LIB: libodin3's objects plus
odin3_test_proc_design, which builds P1) instead of libodin3.

PassrunInfoTest (3D Task 8) formats run information with the core's odin3_passrun_info_append
(through odin3_procfix's odin3_test_passrun_info hook) and reads it back with tools/vtr-qor's
`# abc:` parser (qor_netstats.abc_header), so the two agree on which values need quotes.
"""

from __future__ import annotations

import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from typing import TYPE_CHECKING

import qor_netstats

REPO_ROOT = Path(__file__).resolve().parents[2]
VTR_LIB = REPO_ROOT / "lib" / "vtr.o3lib"
HAND_BODY = REPO_ROOT / "tests" / "golden" / "blif" / "hand_body.blif"
HAND_TOP_LINE = 2  # `.model top`
HAND_N1_LINE = 5  # `.names a b n1`
HAND_SUB_LINE = 18  # `.subckt sub ...` (u_sub)
# Negative fixtures that read_blif rejects by design: 3E preflight E8 (a cover mixing on-set and
# off-set rows), the multi-driver nets (elsif_both_defined, the Odin II original and fuzz seed
# blif003; 3E's neg_e12 and C9; 3A's flatten fixture), and the files of 3E's VPR negative corpus,
# tests/vtr/blif-bad, that our reader refuses as VPR does. They test a refusal, not the walk.
READ_REJECTS = {
    "neg_e8_mixed.in.blif",
    "elsif_both_defined.odin.blif",
    "neg_e12_multidriver.in.blif",
    "c9_two_drivers.blif",
    "multidriver.blif",
    "c5_row_width.blif",
    "c6_init4.blif",
    "c7_no_port.blif",
    "c7_twice.blif",
    "c7_wide_port.blif",
    "c7_ram_data_bit.blif",
}

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False
REQUIRE_CFFI = os.environ.get("ODIN3_REQUIRE_CFFI") == "1"
PROC_LIB = os.environ.get("ODIN3_PROC_LIB")
# Task 3's P1 as odin3_proc_print writes it: its SOURCE record is t.v line 1, and it is node 1.
P1_TEXT = (
    "process $proc$t.v:1$1\n"
    "  assign \\t0 \\q\n"
    "  assign \\t0 \\d\n"
    "  sync posedge \\clk\n"
    "    update \\q \\t0\n"
    "end\n"
)

if TYPE_CHECKING or HAVE_CFFI:
    import walk
    from odin3 import Design, Module, Node, Odin3, Odin3Error, Source, cdef_from_header


def fixtures() -> list[Path]:
    """Every committed BLIF fixture under tests/ that read_blif accepts, smallest first."""
    found = sorted(p for p in (REPO_ROOT / "tests").rglob("*.blif") if p.name not in READ_REJECTS)
    return sorted(found, key=lambda path: (path.stat().st_size, str(path)))


def techlibs_for(path: Path) -> list[Path]:
    """Tech-library fixtures instantiate cells of lib/vtr.o3lib; the others need none."""
    return [VTR_LIB] if path.parent.name == "techlib" else []


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB"), "ODIN3_LIB not set (run CTest python_walk)")
class PythonWalkTest(unittest.TestCase):
    odin3: Odin3

    @classmethod
    def setUpClass(cls) -> None:
        if not HAVE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        cls.odin3 = Odin3()
        # Readers log timings and check warnings at INFO/WARN; keep the test output to errors.
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_ERROR)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_INFO)

    def read(self, path: Path) -> Design:
        return Design.read_blif(path, techlibs=techlibs_for(path), odin3=self.odin3)

    def test_walk_equals_stats_on_every_fixture(self) -> None:
        paths = fixtures()
        self.assertGreater(len(paths), 100)
        compared = 0
        for path in paths:
            with self.subTest(fixture=str(path.relative_to(REPO_ROOT))), self.read(path) as design:
                from_stats = walk.stats_lines(design)
                self.assertGreater(len(from_stats), 1)
                self.assertEqual(walk.walk_lines(design), from_stats)
                compared += 1
        self.assertEqual(compared, len(paths))
        print(f"python_walk: {compared}/{len(paths)} fixtures, walk == stats")

    def test_pins_and_nets_agree_on_every_fixture(self) -> None:
        for path in fixtures():
            with self.subTest(fixture=path.name), self.read(path) as design:
                for module in design.modules():
                    self.check_module_connectivity(module)

    def check_module_connectivity(self, module: Module) -> None:
        """Every connected pin is on its net; every net pin points back at the net."""
        for node in module.nodes():
            pins = list(node.pins())
            self.assertEqual(len(pins), sum(port.width for port in node.ports()))
            for pin in pins:
                self.assertEqual(pin.node(), node)
                net = pin.net()
                if net is not None:
                    self.assertIn(pin, net.pins())
        for net in module.nets():
            drivers = net.drivers()
            self.assertEqual(net.driver(), drivers[0] if drivers else None)
            for pin in net.pins():
                self.assertEqual(pin.net(), net)
            for pin in drivers:
                self.assertIn(pin.direction(), ("out", "inout"))

    def test_counts_agree_with_abi_counts(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.top()
            assert top is not None
            self.assertEqual(top.name, "top")
            self.assertEqual(len(list(top.nodes())), top.node_count())
            self.assertEqual(len(list(top.nets())), top.net_count())
            self.assertEqual(len(list(top.wires())), top.wire_count())
            # bb is a .blackbox model: a cell type, not a module
            self.assertEqual([m.name for m in design.modules()], ["top", "sub"])

    def test_provenance_from_python(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.module("top")
            assert top is not None
            sub = top.node("u_sub")
            assert sub is not None
            self.assertEqual(sub.type_name, "sub")
            first = sub.sources()[0]
            self.assertEqual(
                first, Source(str(HAND_BODY), HAND_SUB_LINE, 1, HAND_SUB_LINE, first.end_col)
            )
            self.assertEqual(top.sources()[0].line, HAND_TOP_LINE)
            found = design.objects_at(str(HAND_BODY), HAND_SUB_LINE)
            self.assertIn(sub, [hit.obj for hit in found])
            self.assertTrue(all(hit.live for hit in found))
            self.assertIn(
                top, [hit.obj for hit in design.objects_at(str(HAND_BODY), HAND_TOP_LINE)]
            )
            self.assertEqual(design.objects_at("nope.blif", HAND_SUB_LINE), [])

    def test_provenance_of_every_node_finds_it_again(self) -> None:
        with self.read(HAND_BODY) as design:
            for module in design.modules():
                for node in module.nodes():
                    sources = node.sources()
                    if not sources:
                        continue  # port nodes and implicit objects may have none
                    hits = design.objects_at(sources[0].file, sources[0].line)
                    self.assertIn(node, [hit.obj for hit in hits])

    def test_driver_params_and_ports(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.module("top")
            assert top is not None
            net = top.net("n1")
            assert net is not None
            driver = net.driver()
            assert driver is not None
            cover = driver.node()
            self.assertEqual(cover.type_name, "$sop")
            self.assertEqual(cover.granularity, "bit")
            self.assertEqual(cover.sources()[0].line, HAND_N1_LINE)
            params = cover.params()
            self.assertIn("11 1\n", params.values())
            self.assertEqual(driver.direction(), "out")
            self.assertEqual([port.direction for port in cover.ports()].count("out"), 1)
            self.assertEqual(top.port_count(), len(top.ports()))
            self.assertEqual([port.wire.name for port in top.ports()][:3], ["a", "b", "clk"])

    def test_attributes(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.module("top")
            assert top is not None
            sub = top.node("u_sub")
            assert sub is not None
            # .attr/.param lines become prefixed string attributes; a repeated key keeps its last
            self.assertEqual(sub.attr("blif.attr:src"), '"top.v:4"')
            self.assertEqual(sub.attr("blif.param:P"), "01 01")
            self.assertIsNone(sub.attr("nope"))
            sub.set_attr("note", "from python")
            self.assertEqual(sub.attr("note"), "from python")
            top.set_attr("note", "module")
            self.assertEqual(top.attr("note"), "module")

    def test_errors_raise(self) -> None:
        with Design(self.odin3) as design:
            with self.assertRaises(Odin3Error) as ctx:
                design.run_pass("no_such_pass")
            self.assertEqual(ctx.exception.status, self.odin3.lib.ODIN3_ERR_INVALID_ARG)
            with self.assertRaises(Odin3Error) as ctx:
                design.run_pass("read_blif", "/nonexistent/x.blif")
            self.assertEqual(ctx.exception.status, self.odin3.lib.ODIN3_ERR_IO)
            self.assertIsNone(design.top())
            self.assertEqual(list(design.modules()), [])

    def test_script_passes_and_aliases(self) -> None:
        self.assertEqual(self.odin3.passes()[:2], ["read_blif", "read_techlib"])
        with Design(self.odin3) as design:
            design.run_script(f'read_blif "{HAND_BODY}"; compact; stats')
            top = design.top()
            assert top is not None
            self.assertEqual(top.node_count(), len(list(top.nodes())))
            for net in top.nets():
                self.assertNotIn(net.name, net.aliases())
            with self.assertRaises(Odin3Error) as ctx:
                design.run_script("no_such_pass")
            self.assertEqual(ctx.exception.status, self.odin3.lib.ODIN3_ERR_PARSE)

    def test_pass_runs(self) -> None:
        """The pass-run table through the cdef (3D Task 8): name, trimmed arguments, information."""
        ffi = self.odin3.ffi
        with Design(self.odin3) as design:
            design.run_script(f'read_blif "{HAND_BODY}";  stats  ')
            count = int(design.get("odin3_design_get_passrun_count", "uint32_t"))
            self.assertEqual(count, 3)
            out = [ffi.new("const char **") for _ in range(3)]
            rows = []
            for run in range(1, count):
                design.call("odin3_design_get_passrun", run, *out)
                rows.append(tuple(self.odin3.text(ptr[0]) for ptr in out))
            self.assertEqual(rows, [("read_blif", f'"{HAND_BODY}"', ""), ("stats", "", "")])
            with self.assertRaises(Odin3Error) as ctx:
                design.call("odin3_design_get_passrun", count, *out)
            self.assertEqual(ctx.exception.status, self.odin3.lib.ODIN3_ERR_INVALID_ARG)

    def test_capture_log_nests(self) -> None:
        odin3 = self.odin3
        with odin3.capture_log() as outer:
            odin3.call("odin3_log_write", odin3.lib.ODIN3_LOG_INFO, b"before")
            with odin3.capture_log() as inner:
                odin3.call("odin3_log_write", odin3.lib.ODIN3_LOG_INFO, b"inside")
            odin3.call("odin3_log_write", odin3.lib.ODIN3_LOG_INFO, b"after")
            with self.read(HAND_BODY) as design:
                self.assertGreater(len(walk.stats_lines(design)), 1)  # itself a capture
            odin3.call("odin3_log_write", odin3.lib.ODIN3_LOG_INFO, b"last")
        self.assertEqual([rec.message for rec in inner], ["inside"])
        self.assertEqual([rec.message for rec in outer][:2], ["before", "after"])
        self.assertEqual(outer[-1].message, "last")
        self.assertEqual(int(odin3.lib.odin3_log_get_level()), int(odin3.lib.ODIN3_LOG_ERROR))

    def test_abc_script_setter(self) -> None:
        """3D Task 13: odin3_pass_set_abc_script is in the cdef (lib resolves it); NULL clears."""
        odin3 = self.odin3
        ok = int(odin3.lib.ODIN3_OK)
        self.assertEqual(int(odin3.lib.odin3_pass_set_abc_script(b"map.abc")), ok)
        self.assertEqual(int(odin3.lib.odin3_pass_set_abc_script(odin3.ffi.NULL)), ok)

    def test_engine_abi_in_cdef(self) -> None:
        """3D Task 4: the cdef holds the ABC engine section, and its function resolves."""
        odin3 = self.odin3
        self.assertIn("odin3_abc_engine_register", cdef_from_header())
        self.assertEqual(int(odin3.lib.ODIN3_ERR_ENGINE), 8)
        self.assertEqual(int(odin3.lib.ODIN3_STATUS_COUNT), 9)
        self.assertEqual(int(odin3.lib.ODIN3_ABC_ENGINE_VERSION), 1)
        self.assertEqual(odin3.ffi.sizeof("odin3_abc_fail"), 264)
        self.assertEqual(odin3.status_string(int(odin3.lib.ODIN3_ERR_ENGINE)), "ODIN3_ERR_ENGINE")
        with odin3.capture_log() as log:  # NULL is refused and registers nothing
            status = odin3.lib.odin3_abc_engine_register(odin3.ffi.NULL)
        self.assertEqual(int(status), int(odin3.lib.ODIN3_ERR_INVALID_ARG))
        self.assertEqual(
            [rec.message for rec in log], ["odin3_abc_engine_register: invalid argument engine"]
        )

    def test_walk_script(self) -> None:
        out = io.StringIO()
        with redirect_stdout(out):
            code = walk.main(["--check", str(HAND_BODY)])
        self.assertEqual(code, 0)
        text = out.getvalue()
        self.assertIn("module top: ports 6, nodes 18, nets 14, wires 6", text)
        self.assertIn("module top: cell $sop 4", text)
        self.assertIn("walk == stats", text)

    def test_node_is_hashable_and_equal_by_id(self) -> None:
        with self.read(HAND_BODY) as design:
            top = design.top()
            assert top is not None
            first = next(iter(top.nodes()))
            again = next(iter(top.nodes()))
            self.assertEqual(first, again)
            self.assertEqual(len({first, again}), 1)
            self.assertIsInstance(first, Node)


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(PROC_LIB, "ODIN3_PROC_LIB not set (run CTest python_walk)")
class ProcWalkTest(unittest.TestCase):
    """The $proc node through the ABI: the process count, value kind PROC and the BODY text."""

    odin3: Odin3

    @classmethod
    def setUpClass(cls) -> None:
        if not HAVE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        assert PROC_LIB is not None
        cls.odin3 = Odin3(Path(PROC_LIB))
        cls.odin3.ffi.cdef("odin3_design *odin3_test_proc_design(void);")
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_ERROR)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.odin3.call("odin3_log_set_level", cls.odin3.lib.ODIN3_LOG_INFO)

    def design(self) -> Design:
        """A design of an empty module e and a module m holding P1 (proc_fixture_build_design)."""
        return Design(self.odin3, handle=self.odin3.lib.odin3_test_proc_design())

    def test_process_count_kind_and_text(self) -> None:
        with self.design() as design:
            empty = design.module("e")
            module = design.module("m")
            assert empty is not None and module is not None
            self.assertEqual((empty.proc_count(), module.proc_count()), (0, 1))
            procs = [node for node in module.nodes() if node.type_name == "$proc"]
            self.assertEqual([node.id for node in procs], [1])
            node = procs[0]
            self.assertEqual([node.param_kind(i) for i in range(3)], ["INT", "INT", "PROC"])
            self.assertEqual(node.params(), {"I_WIDTH": 2, "O_WIDTH": 2, "BODY": P1_TEXT})
            with self.assertRaises(Odin3Error) as ctx:
                node.design.get("odin3_node_get_param_int", "int64_t", node.ref(), 2)
            self.assertEqual(ctx.exception.status, self.odin3.lib.ODIN3_ERR_INVALID_ARG)

    def test_walk_prints_the_process(self) -> None:
        with self.design() as design:
            lines = walk.param_lines(design)
            head = "module m: node 1 $proc: BODY PROC"
            self.assertIn("module m: node 1 $proc: I_WIDTH INT 2", lines)
            self.assertIn(head, lines)
            at = lines.index(head) + 1
            body = [f"    {row}" for row in P1_TEXT.splitlines()]
            self.assertEqual(lines[at : at + len(body)], body)
            self.assertEqual(walk.walk_lines(design), walk.stats_lines(design))
            self.assertIn("module m: cell $proc 1", walk.walk_lines(design))


# Script paths as a user may give them (3D §8.5: recorded as given), each with bytes that a
# blank-splitting reader could mistake for a separator: Python's str.isspace() is true for VT, FF,
# 0x1c-0x1f, U+0085 and U+00A0 as well as space, tab, CR and LF.
SCRIPT_PATHS = [
    "lib/abc/vtr.abc",
    "builtin:lib/abc/vtr.abc",
    "dir with blanks/a.abc",
    "tab\there.abc",
    "nbsp\u00a0here.abc",
    "nel\u0085here.abc",
    "vt\x0bhere.abc",
    "ff\x0chere.abc",
    "us\x1fhere.abc",
    "del\x7fhere.abc",
    'quote"and\\backslash.abc',
]
SHA256 = "0123456789abcdef" * 4
ABC_COMMIT = "0123456789abcdef0123456789abcdef01234567"
INFO_CAP = 4096


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(PROC_LIB, "ODIN3_PROC_LIB not set (run CTest python_walk)")
class PassrunInfoTest(unittest.TestCase):
    """The core's run-information words read back by qor_netstats (3D Task 8, fix round 1)."""

    odin3: Odin3

    @classmethod
    def setUpClass(cls) -> None:
        if not HAVE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        assert PROC_LIB is not None
        cls.odin3 = Odin3(Path(PROC_LIB))
        cls.odin3.ffi.cdef(
            "odin3_status odin3_test_passrun_info(const char *const *pairs, uint32_t count,"
            " char *out, size_t cap);"
        )

    def info(self, words: list[tuple[str, str]]) -> bytes:
        """The run information the core formats from words, as bytes."""
        ffi = self.odin3.ffi
        keep = [ffi.new("char[]", text.encode()) for word in words for text in word]
        out = ffi.new("char[]", INFO_CAP)
        pairs = ffi.new("const char *[]", keep)
        self.odin3.call("odin3_test_passrun_info", pairs, len(words), out, INFO_CAP)
        return bytes(ffi.string(out))

    def abc_words(self, script: str) -> list[tuple[str, str]]:
        return [
            ("script", script),
            ("sha256", SHA256),
            ("abc", ABC_COMMIT),
            ("K", "6"),
            ("exchange", "blif"),
            ("depth", "3"),
        ]

    def test_qor_netstats_reads_every_script_path(self) -> None:
        for script in SCRIPT_PATHS:
            with self.subTest(script=script):
                line = self.info(self.abc_words(script))
                bare = all("!" <= c <= "~" and c not in '"\\' for c in script)
                self.assertEqual(line.startswith(b'script="'), not bare)
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "d.blif"
                    path.write_bytes(
                        b"# Odin III 0.0.0 write_blif --vtr (K=6, names short)\n"
                        b"# abc: " + line + b"\n\n.model top\n.end\n"
                    )
                    header = qor_netstats.abc_header(path)
                assert header is not None
                self.assertEqual(header.script, script)
                self.assertEqual((header.sha256, header.abc), (SHA256, ABC_COMMIT))
                self.assertEqual((header.k, header.exchange, header.depth), ("6", "blif", 3))

    def test_one_line_values_only(self) -> None:
        for script in ("cr\rhere.abc", "lf\nhere.abc"):
            with self.subTest(script=script), self.odin3.capture_log() as log:
                with self.assertRaises(Odin3Error) as ctx:
                    self.info(self.abc_words(script))
                self.assertEqual(ctx.exception.status, self.odin3.lib.ODIN3_ERR_INVALID_ARG)
                self.assertEqual(
                    [rec.message for rec in log],
                    ["odin3_passrun_info_append: invalid argument value"],
                )


if __name__ == "__main__":
    unittest.main()

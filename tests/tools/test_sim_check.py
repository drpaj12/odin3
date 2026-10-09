"""Tests for tools/sim-check: the testbench generator tb.sh and the sim-check harness.

They need odin3-sim-vectors (ODIN3_SIM_VECTORS), ABC (ODIN3_ABC), Yosys (ODIN3_YOSYS, else the
workspace build or PATH) and Icarus (iverilog/vvp on PATH); without one of them the tests skip,
unless ODIN3_REQUIRE_SIM_CHECK=1 (the CTest run when CMake found every tool), which makes them fail.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import helpers

HERE = helpers.REPO_ROOT / "tools" / "sim-check"
SIM_CHECK = HERE / "sim-check"
TB = HERE / "tb.sh"
BLIF = helpers.REPO_ROOT / "tests" / "golden" / "blif"
TECHLIB = helpers.REPO_ROOT / "tests" / "golden" / "techlib"
VTR_LIB = helpers.REPO_ROOT / "lib" / "vtr.o3lib"


def _yosys() -> str:
    env = os.environ.get("ODIN3_YOSYS", "")
    if env:
        return env
    local = Path.home() / "odin3-ws" / "external" / "yosys" / "build" / "yosys"
    return str(local) if local.is_file() else shutil.which("yosys") or ""


DRIVER = os.environ.get("ODIN3_SIM_VECTORS", "")
ABC = os.environ.get("ODIN3_ABC", "")
YOSYS = _yosys()
IVERILOG = shutil.which("iverilog") or ""
VVP = shutil.which("vvp") or ""
MISSING = [n for n, p in [("ODIN3_SIM_VECTORS", DRIVER), ("ODIN3_ABC", ABC), ("yosys", YOSYS),
                          ("iverilog", IVERILOG), ("vvp", VVP)] if not p]
REQUIRE = os.environ.get("ODIN3_REQUIRE_SIM_CHECK") == "1"
needs_tools = unittest.skipIf(bool(MISSING) and not REQUIRE, f"missing {', '.join(MISSING)}")

# Ports in every grouping the reader knows: a vector (v[0..2]), bits that stay scalars
# (a[0] b a[1]), a clock used as data, and output vector/scalar mixes; one register on each edge.
PORTS_BLIF = """\
.model top
.inputs a[0] b a[1] v[0] v[1] v[2] clk
.outputs y[0] y[1] z q[0] q[1] c
.names a[0] v[2] y[0]
11 1
.names b a[1] v[1] y[1]
1-1 1
-11 1
.names v[0] clk z
10 1
.names v[2] d
1 1
.latch d q[0] re clk 3
.latch q[0] q[1] {edge} clk 1
.names clk c
1 1
.end
"""


def run(cmd: list[str | Path]) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(c) for c in cmd], capture_output=True, text=True, check=False,
                          timeout=600)


def data_lines(text: str) -> list[str]:
    return [line for line in text.splitlines() if line and not line.startswith("#")]


@needs_tools
class TbTest(unittest.TestCase):
    """tb.sh: the reference testbench reproduces the driver's line format exactly."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="tb-test."))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp)

    def reference(self, blif: Path, flavor: str, vectors: str, sop: bool = False) -> list[str]:
        """The reference lines; sop: Yosys keeps .names as $sop cells, run by models/sop.v."""
        vec = self.tmp / "vec.txt"
        vec.write_text(vectors)
        ref = self.tmp / "ref.v"
        if flavor == "abc":
            res = run([ABC, "-q", f"read_blif {blif}; write_verilog {ref}"])
        else:
            mode = "-sop -wideports" if sop else "-wideports"
            script = f"read_blif {mode} {blif}; write_verilog -noattr {ref}"
            res = run([YOSYS, "-q", "-p", script])
            if sop:
                self.assertIn("$sop", ref.read_text())
                ref.write_text(ref.read_text() + (HERE / "models" / "sop.v").read_text())
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        tb = run([TB, "--flavor", flavor, "--blif", blif, "--vectors", vec, "--ref", ref])
        self.assertEqual(tb.returncode, 0, tb.stderr)
        (self.tmp / "tb.v").write_text(tb.stdout)
        mem = self.tmp / "in.mem"
        mem.write_text("".join(line.split()[1] + "\n" for line in data_lines(vectors)))
        comp = run([IVERILOG, "-o", self.tmp / "sim.vvp", ref, self.tmp / "tb.v"])
        self.assertEqual(comp.returncode, 0, comp.stderr)
        sim = run([VVP, "-n", self.tmp / "sim.vvp", f"+vec={mem}"])
        self.assertEqual(sim.returncode, 0, sim.stderr)
        return [line[1:] for line in sim.stdout.splitlines() if line.startswith("=")]

    def ours(self, blif: Path, seed: int = 7) -> str:
        res = run([DRIVER, blif, "--seed", str(seed), "--cycles", "32"])
        self.assertEqual(res.returncode, 0, res.stderr)
        return res.stdout

    def test_abc_reference_reproduces_ff(self) -> None:
        for name in ["ff.odin.blif", "ff.parmys.blif"]:
            with self.subTest(fixture=name):
                vectors = self.ours(BLIF / name)
                self.assertEqual(self.reference(BLIF / name, "abc", vectors), data_lines(vectors))

    def test_port_grouping_both_flavors(self) -> None:
        blif = self.tmp / "ports.blif"
        blif.write_text(PORTS_BLIF.format(edge="re").replace(" 3\n", " 0\n"))
        vectors = self.ours(blif)
        self.assertIn("# input v 3", vectors)
        self.assertIn("# output q 2", vectors)
        for flavor in ["abc", "yosys"]:
            with self.subTest(flavor=flavor):
                self.assertEqual(self.reference(blif, flavor, vectors), data_lines(vectors))

    def test_sop_model(self) -> None:
        # models/sop.v (Yosys < 0.45 cannot simplemap $sop): ON-set, OFF-set, empty, wide covers.
        ins = " ".join(f"i{k}" for k in range(14))
        blif = self.tmp / "sop.blif"
        blif.write_text(f".model w\n.inputs {ins}\n.outputs y n e b\n.names {ins} y\n"
                        + "1" * 14 + " 1\n" + "0" * 13 + "- 1\n" + "-1-0" * 3 + "-1 1\n"
                        ".names i2 i3 i4 n\n1-0 0\n-11 0\n.names i5 i6 e\n"
                        ".names i7 b\n0 1\n.end\n")
        vectors = self.ours(blif)
        self.assertEqual(self.reference(blif, "yosys", vectors, sop=True), data_lines(vectors))

    def test_negedge_register_with_yosys(self) -> None:
        blif = self.tmp / "ports.blif"
        blif.write_text(PORTS_BLIF.format(edge="fe").replace(" 3\n", " 0\n"))
        vectors = self.ours(blif)
        self.assertEqual(self.reference(blif, "yosys", vectors), data_lines(vectors))

    def test_no_inputs(self) -> None:
        blif = self.tmp / "const.blif"
        blif.write_text(".model k\n.outputs o\n.names o\n1\n.end\n")
        vectors = self.ours(blif)
        self.assertEqual(data_lines(vectors)[0], "0 - 1")
        self.assertEqual(self.reference(blif, "abc", vectors), data_lines(vectors))

    def test_port_count_mismatch_is_an_error(self) -> None:
        vec = self.tmp / "vec.txt"
        vec.write_text(self.ours(BLIF / "ff.odin.blif"))
        blif = self.tmp / "other.blif"
        blif.write_text(".model k\n.inputs a b\n.outputs o\n.names a b o\n11 1\n.end\n")
        ref = self.tmp / "ref.v"
        ref.write_text("module k(a, b, o);\nendmodule\n")
        res = run([TB, "--flavor", "abc", "--blif", blif, "--vectors", vec, "--ref", ref])
        self.assertEqual(res.returncode, 1)
        self.assertIn("ports", res.stderr)

    def test_usage(self) -> None:
        for args in [[], ["--flavor", "vcs"], ["--bogus"]]:
            with self.subTest(args=args):
                res = run([TB, *args])
                self.assertEqual(res.returncode, 2)
                self.assertIn("usage:", res.stderr)


@needs_tools
class SimCheckTest(unittest.TestCase):
    """sim-check: classification, exclusions, comparison and the summary."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="sim-check-test."))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp)

    def check(self, *args: str | Path,
              driver: str | Path = DRIVER) -> subprocess.CompletedProcess[str]:
        return run([SIM_CHECK, "--driver", driver, "--abc", ABC, "--yosys", YOSYS,
                    "--techlib", VTR_LIB, "-m", "0", "--out", self.tmp / "out", *args])

    def table(self, stdout: str) -> dict[str, str]:
        """file basename -> status column."""
        rows: dict[str, str] = {}
        for line in stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[-1].endswith(".blif"):
                rows[Path(parts[-1]).name] = parts[0]
        return rows

    def test_fixtures(self) -> None:
        files = sorted(BLIF.glob("*.*.blif"))
        res = self.check("--fixtures", *files)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual(self.table(res.stdout), {
            "adder_hard_block.parmys.blif": "pass",
            "ansiportlist_2.parmys.blif": "pass",
            "dffsre.parmys.blif": "excluded",
            "elsif_both_defined.odin.blif": "excluded",
            "ff.odin.blif": "pass",
            "ff.parmys.blif": "pass",
            "pow.parmys.blif": "excluded",
        })
        self.assertIn("pass 4, FAIL 0", res.stdout)
        self.assertIn("RESULT: PASS", res.stdout)

    def test_techlib_fixtures(self) -> None:
        files = sorted(TECHLIB.glob("*.01.blif"))
        res = self.check("--fixtures", *files)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        rows = self.table(res.stdout)
        self.assertEqual(rows["multiply.parmys.01.blif"], "pass")
        self.assertEqual(rows["adder.odin.01.blif"], "pass")
        self.assertEqual(rows["single_port_ram.odin.01.blif"], "excluded")
        self.assertIn("yosys", res.stdout)

    def test_edges_choose_the_reference(self) -> None:
        for edge in ["re", "fe"]:
            (self.tmp / f"{edge}.blif").write_text(PORTS_BLIF.format(edge=edge))
        res = self.check("--fixtures", self.tmp / "re.blif", self.tmp / "fe.blif")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertRegex(res.stdout, r"pass\s+abc\s.*re\.blif")
        self.assertRegex(res.stdout, r"pass\s+yosys\s.*fe\.blif")

    def test_negedge_toggle_and_continued_subckt(self) -> None:
        # A falling-edge toggle (x forever if the testbench's clock starts x: x -> 0 is a
        # negedge) and an Odin II-style .subckt split over continuation lines.
        blif = self.tmp / "toggle.blif"
        blif.write_text(".model t\n.inputs clk a b\n.outputs q s\n"
                        ".latch nq q fe clk 2\n.names q nq\n0 1\n"
                        ".subckt adder a[0]=a b[0]=b cin[0]=q\\\n cout[0]=co\\\n sumout[0]=s\n"
                        ".end\n\n.model adder\n.inputs a[0] b[0] cin[0]\n"
                        ".outputs cout[0] sumout[0]\n.blackbox\n.end\n")
        res = self.check("--fixtures", blif)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertRegex(res.stdout, r"pass\s+yosys\s.*toggle\.blif")

    def test_wide_cover_with_yosys(self) -> None:
        # Yosys reads a .names of more than 12 inputs only as a $sop (read_blif -sop), which
        # Icarus runs from models/sop.v; an OFF-set cover and a DEPTH 0 cover too.
        ins = " ".join(f"i{k}" for k in range(14))
        blif = self.tmp / "wide.blif"
        blif.write_text(f".model w\n.inputs {ins} c\n.outputs y s n e\n.names {ins} y\n"
                        + "1" * 14 + " 1\n" + "0" * 13 + "- 1\n"
                        ".names i2 i3 i4 n\n1-0 0\n-11 0\n.names i5 i6 e\n"
                        ".subckt adder a=i0 b=i1 cin=c sumout=s\n.end\n\n"
                        ".model adder\n.inputs a b cin\n.outputs cout sumout\n.blackbox\n.end\n")
        res = self.check("--fixtures", blif)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertRegex(res.stdout, r"pass\s+yosys\s.*wide\.blif")

    def test_detects_a_wrong_output(self) -> None:
        liar = self.tmp / "liar"
        # Flips the first output bit of cycle 5.
        flip = '{ $3 = (substr($3, 1, 1) == "0" ? "1" : "0") substr($3, 2); print }'
        liar.write_text(f"#!/usr/bin/env bash\n'{DRIVER}' \"$@\" | "
                        f"awk '/^#/ || $1 != 5 {{ print; next }} {flip}'\n")
        liar.chmod(0o755)
        res = self.check("--fixtures", BLIF / "ff.parmys.blif", driver=liar)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("FAIL", res.stdout)
        self.assertIn("seed 1 cycle 5", res.stdout)
        self.assertIn("RESULT: FAIL", res.stdout)

    def test_simulator_error_is_a_failure(self) -> None:
        bad = self.tmp / "loop.blif"
        bad.write_text(".model l\n.inputs a\n.outputs o\n.names a o x\n11 1\n"
                       ".names x o\n1 1\n.end\n")
        res = self.check("--fixtures", bad)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("sim-error", res.stdout)

    def test_usage(self) -> None:
        res = run([SIM_CHECK, "--bogus"])
        self.assertEqual(res.returncode, 2)
        self.assertIn("usage:", res.stderr)


if __name__ == "__main__":
    unittest.main()

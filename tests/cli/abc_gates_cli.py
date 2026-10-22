"""abc_gates_cli: `abc --gates` end to end with the real linked ABC (gatemap plan Task 13).

Usage: abc_gates_cli.py ODIN3 EQUIV_CHECK OUT_DIR

Each case writes a Verilog design to its own work directory under OUT_DIR and runs

    read_techlib L; read_verilog --nolatches t.v; hierarchy -auto; proc; opt; lower;
    write_blif --gates-as-names rtl.blif; abc --gates L; dfflibmap L;
    write_blif --gates-as-names gates.blif; write_verilog gates.v; dump_locs --types locs.txt

for L = generic_gates and cmos6 (lib/*.o3lib), then `equiv-check rtl.blif gates.blif` (exit 0).
Review Focus 2 (a constant output: a TIE0 cell with generic_gates, the literal 1'b0 with cmos6) and
4 (a user script ending in `&nf`) are test_const_output_tie_vs_literal and test_nf_script.
equiv-check cannot read a flip-flop clocked by an internal net, so the cmos6 negedge case (a DFF
behind dfflibmap's clock inverter) checks the inverter and pins equiv-check's refusal instead.
"""

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from dataclasses import dataclass
from pathlib import Path

LIB_DIR = Path(__file__).resolve().parents[2] / "lib"
LIBRARIES = ("generic_gates", "cmos6")
ODIN3 = ""
EQUIV = ""
OUT_DIR = Path()

COMB = """module t(input a, input b, input c, output y, output z);
  assign y = (a & b) ^ c;
  assign z = ~a;
endmodule
"""
SEQ = """module t(input clk, input a, input b, input c, output reg q);
  always @(posedge clk) q <= (a & b) ^ c;
endmodule
"""
NEG = """module t(input clk, input a, input b, output reg q);
  always @(negedge clk) q <= a ^ b;
endmodule
"""
CONST = """module t(input a, output y);
  assign y = a & ~a;
endmodule
"""
NF_SCRIPT = "&st\n&nf\n"
LUT_SCRIPT = "strash\nif -K 6\n"
INSTANCE = re.compile(r"^  ([A-Za-z_$][\w$]*) +(\\\S+|[A-Za-z_][\w$]*) *\(")


@dataclass
class Run:
    """One flow: its work directory and the odin3 process."""

    work: Path
    proc: subprocess.CompletedProcess[str]

    @property
    def text(self) -> str:
        return self.proc.stdout + self.proc.stderr

    def equiv(self) -> subprocess.CompletedProcess[str]:
        argv = [EQUIV, str(self.work / "rtl.blif"), str(self.work / "gates.blif")]
        return subprocess.run(argv, capture_output=True, text=True, check=False)

    def nodes(self) -> list[tuple[str, str]]:
        """(type, SRC) of every node line of locs.txt (`mod|node:TYPE|name|SRC`)."""
        out = []
        for line in (self.work / "locs.txt").read_text(encoding="utf-8").splitlines():
            parts = line.split("|", 3)
            if len(parts) >= 3 and parts[1].startswith("node:"):
                out.append((parts[1][len("node:"):], parts[3] if len(parts) == 4 else ""))
        return out

    def instances(self) -> list[str]:
        """The cell type of every instance in gates.v."""
        text = (self.work / "gates.v").read_text(encoding="utf-8")
        return [m.group(1) for m in map(INSTANCE.match, text.splitlines()) if m is not None]


def library_cells(lib: str) -> set[str]:
    text = (LIB_DIR / f"{lib}.o3lib").read_text(encoding="utf-8")
    return set(re.findall(r"^cell +(\S+)", text, flags=re.MULTILINE))


@dataclass
class Opts:
    """How a flow differs from the plain one: no opt, lower's and abc's extra arguments, no
    read_techlib, another library name for abc and dfflibmap."""

    opt: bool = True
    lower: str = ""
    abc: str = ""
    techlib: bool = True
    target: str = ""


def flow(case: str, lib: str, verilog: str, opts: Opts | None = None) -> Run:
    opts = opts or Opts()
    target = opts.target or lib
    work = OUT_DIR / f"{case}.{lib}"
    work.mkdir(parents=True, exist_ok=True)
    (work / "t.v").write_text(verilog, encoding="utf-8")
    steps = [f"read_techlib {LIB_DIR / (lib + '.o3lib')}"] if opts.techlib else []
    steps += [f"read_verilog --nolatches {work / 't.v'}", "hierarchy -auto", "proc"]
    steps += ["opt"] if opts.opt else []
    steps += [f"lower {opts.lower}".strip(), f"write_blif --gates-as-names {work / 'rtl.blif'}",
              f"abc --gates {target} {opts.abc}".strip(), f"dfflibmap {target}",
              f"write_blif --gates-as-names {work / 'gates.blif'}",
              f"write_verilog {work / 'gates.v'}", f"dump_locs --types {work / 'locs.txt'}"]
    proc = subprocess.run([ODIN3, "-p", "; ".join(steps)], capture_output=True, text=True,
                          check=False)
    return Run(work, proc)


def write_script(name: str, text: str) -> Path:
    path = OUT_DIR / name
    path.write_text(text, encoding="utf-8")
    return path


class AbcGatesCli(unittest.TestCase):
    def mapped(self, case: str, lib: str, verilog: str, opts: Opts | None = None) -> Run:
        run = flow(case, lib, verilog, opts)
        self.assertEqual(run.proc.returncode, 0, run.text[-2000:])
        return run

    def assert_equivalent(self, run: Run) -> None:
        verdict = run.equiv()
        self.assertEqual(verdict.returncode, 0, (verdict.stdout + verdict.stderr)[-1000:])

    def test_comb_equivalent(self) -> None:
        for lib in LIBRARIES:
            with self.subTest(lib=lib):
                self.assert_equivalent(self.mapped("comb", lib, COMB))

    def test_seq_equivalent(self) -> None:
        state = {"generic_gates": "DFFP", "cmos6": "DFF"}
        for lib in LIBRARIES:
            with self.subTest(lib=lib):
                run = self.mapped("seq", lib, SEQ)
                self.assert_equivalent(run)
                self.assertIn(state[lib], run.instances())

    def test_negedge_generic_equivalent(self) -> None:
        run = self.mapped("neg", "generic_gates", NEG, Opts(lower="--neg-edge keep"))
        self.assert_equivalent(run)
        self.assertIn("DFFN", run.instances())

    def test_negedge_cmos6_inverter(self) -> None:
        run = self.mapped("neg", "cmos6", NEG, Opts(lower="--neg-edge keep"))
        text = (run.work / "gates.v").read_text(encoding="utf-8")
        on_clock = re.findall(r"^  INV \S+ \(.*\n    \.A\(clk\),", text, flags=re.MULTILINE)
        self.assertEqual(len(on_clock), 1, text)
        self.assertIn("dfflibmap: library 'cmos6': 1 cells mapped (DFF x1), 1 clock inverters",
                      run.text)
        self.assertEqual(run.instances().count("DFF"), 1)
        verdict = run.equiv()
        self.assertEqual(verdict.returncode, 2, verdict.stdout + verdict.stderr)
        self.assertIn("is clocked by internal net", verdict.stdout + verdict.stderr)

    def test_nf_script(self) -> None:
        for lib in LIBRARIES:
            with self.subTest(lib=lib):
                script = write_script("nf.abc", NF_SCRIPT)
                run = self.mapped("nf", lib, COMB, Opts(abc=f"--script {script}"))
                self.assert_equivalent(run)
                self.assertTrue(run.instances())
                self.assertLessEqual(set(run.instances()), library_cells(lib))

    def test_const_output_tie_vs_literal(self) -> None:
        generic = self.mapped("const", "generic_gates", CONST, Opts(opt=False))
        self.assert_equivalent(generic)
        self.assertIn("TIE0", [kind for kind, _ in generic.nodes()])
        cmos6 = self.mapped("const", "cmos6", CONST, Opts(opt=False))
        self.assert_equivalent(cmos6)
        self.assertFalse([kind for kind, _ in cmos6.nodes() if kind.startswith("TIE")])
        self.assertIn("1'b0", (cmos6.work / "gates.v").read_text(encoding="utf-8"))

    def test_verilog_only_library_cells(self) -> None:
        for lib in LIBRARIES:
            for case, verilog in (("comb", COMB), ("seq", SEQ)):
                with self.subTest(lib=lib, case=case):
                    run = self.mapped(case, lib, verilog)
                    self.assertTrue(run.instances())
                    self.assertLessEqual(set(run.instances()), library_cells(lib))

    def test_every_gate_has_location(self) -> None:
        for lib in LIBRARIES:
            cells = library_cells(lib)
            for case, verilog in (("comb", COMB), ("seq", SEQ), ("const", CONST)):
                with self.subTest(lib=lib, case=case):
                    run = self.mapped(case, lib, verilog, Opts(opt=case != "const"))
                    # library cells and the splice's $_CONST0_/$_CONST1_/$_BUF_ cells
                    gates = [(kind, src) for kind, src in run.nodes()
                             if kind in cells or kind.startswith("$_")]
                    self.assertTrue(gates)
                    for kind, src in gates:
                        self.assertNotEqual(src, "", kind)

    def refused(self, run: Run, text: str) -> None:
        self.assertNotEqual(run.proc.returncode, 0)
        self.assertIn(text, run.text)

    def test_no_techlib_error(self) -> None:
        self.refused(flow("err_none", "generic_gates", COMB, Opts(techlib=False)),
                     "abc: --gates: library 'generic_gates' is not loaded; read it with "
                     "read_techlib first")

    def test_unknown_library_error(self) -> None:
        self.refused(flow("err_unknown", "generic_gates", COMB, Opts(target="nosuch")),
                     "abc: --gates: library 'nosuch' is not loaded; read it with read_techlib "
                     "first")

    def test_lut_script_error(self) -> None:
        script = write_script("lut.abc", LUT_SCRIPT)
        self.refused(flow("err_lut", "generic_gates", COMB, Opts(abc=f"--script {script}")),
                     "the script does not end gate-mapped; with --gates end it with a gate "
                     "mapper ('map' or '&nf')")


def main() -> int:
    global ODIN3, EQUIV, OUT_DIR
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    ODIN3, EQUIV, OUT_DIR = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    result = unittest.main(argv=[sys.argv[0], "-v"], exit=False).result
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())

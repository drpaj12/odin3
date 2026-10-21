"""Phase 7, Task 9 (spec 3.5): plugins/python/re_round.py finds an adder in an anonymous BLIF.

A micro adder goes through the CLI (`proc; opt; lower; abc --lut 3`) to a BLIF; every signal name is
then replaced by a generated one. re_round reads that BLIF, guesses the adder cone, proves it with
`cone_check`, `raise`s it and writes Verilog (`$add`) and JSON; the JSON, lowered and LUT-mapped, is
equivalent to the anonymous input (equiv-check). Run through CTest `python_re_round`, which sets
ODIN3_LIB, ODIN3_CLI and ODIN3_ABC. Skipped where cffi or ODIN3_LIB is missing, unless
ODIN3_REQUIRE_CFFI=1.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import unittest
from pathlib import Path

from . import helpers

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False

REPO_ROOT = Path(__file__).resolve().parents[2]
RE_ROUND = REPO_ROOT / "plugins" / "python" / "re_round.py"
EQUIV = REPO_ROOT / "tools" / "equiv-check" / "equiv-check"
CLI = os.environ.get("ODIN3_CLI", "")


def odin3(script: str) -> str:
    """Run a pass script through the CLI; its output when it succeeds."""
    run = subprocess.run([CLI, "-p", script], capture_output=True, text=True, check=False)
    if run.returncode != 0:
        raise AssertionError(f"odin3 -p {script!r}: {run.stderr[-300:]}")
    return run.stderr + run.stdout


def anonymize(text: str) -> str:
    """The BLIF with every signal renamed n0, n1, ... in order of appearance."""
    names: dict[str, str] = {}
    out: list[str] = []
    for line in text.splitlines():
        words = line.split()
        if words and words[0] in (".inputs", ".outputs", ".names"):
            for i, word in enumerate(words[1:], 1):
                words[i] = names.setdefault(word, f"n{len(names)}")
            line = " ".join(words)
        out.append(line)
    return "\n".join(out) + "\n"


@unittest.skipUnless(HAVE_CFFI or os.environ.get("ODIN3_REQUIRE_CFFI") == "1", "cffi not installed")
@unittest.skipUnless(os.environ.get("ODIN3_LIB") and CLI, "ODIN3_LIB/ODIN3_CLI not set")
class ReRoundTest(unittest.TestCase):
    def blif_of(self, verilog: str, name: str, scratch: Path) -> Path:
        src = scratch / f"{name}.v"
        src.write_text(verilog, encoding="utf-8")
        ref = scratch / f"{name}.blif"
        odin3(f'read_verilog "{src}"; proc; opt; lower; abc --lut 3; write_blif "{ref}"')
        anon = scratch / f"{name}_anon.blif"
        anon.write_text(anonymize(ref.read_text(encoding="utf-8")), encoding="utf-8")
        return anon

    def round(self, anon: Path, scratch: Path) -> tuple[subprocess.CompletedProcess[str], Path]:
        out = scratch / "out"
        run = subprocess.run([sys.executable, str(RE_ROUND), str(anon), f"{out}.v", "--json",
                              f"{out}.json"], capture_output=True, text=True, check=False)
        return run, out

    def check_adder(self, width: int) -> None:
        scratch = helpers.scratch(self, "re_round_")
        verilog = (f"module add(input [{width - 1}:0] a, input [{width - 1}:0] b,"
                   f" output [{width - 1}:0] y); assign y = a + b; endmodule\n")
        anon = self.blif_of(verilog, "add", scratch)
        run, out = self.round(anon, scratch)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn(f"{width}-bit adder", run.stdout)
        self.assertIn("equivalent", run.stdout)
        text = Path(f"{out}.v").read_text(encoding="utf-8")
        self.assertIn("$add", text)  # the proof's cell, kept as a module attribute
        self.assertRegex(text, r"assign \{.*\} = \{.*\} \+ \{.*\};")  # the word-level adder
        mapped = scratch / "raised.blif"
        odin3(f'read_json --top add "{out}.json"; lower; abc --lut 4; write_blif "{mapped}"')
        verdict = subprocess.run([str(EQUIV), str(anon), str(mapped)], capture_output=True,
                                 text=True, check=False)
        self.assertEqual(verdict.returncode, 0, verdict.stdout + verdict.stderr)

    def test_re_round_finds_add(self) -> None:
        self.check_adder(4)

    def test_re_round_finds_add_8bit(self) -> None:
        self.check_adder(8)

    def test_load_abc_twice(self) -> None:
        import odin3 as binding

        lib = binding.Odin3()
        lib.load_abc()
        lib.load_abc()  # a second call does nothing

    def test_re_round_no_adder(self) -> None:
        scratch = helpers.scratch(self, "re_round_")
        anon = self.blif_of("module m(input [3:0] a, input [3:0] b, output [3:0] y);"
                            " assign y = a & b; endmodule\n", "and", scratch)
        run, out = self.round(anon, scratch)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertRegex(run.stdout + run.stderr, re.compile("no adder"))
        self.assertFalse(Path(f"{out}.v").exists())


if __name__ == "__main__":
    unittest.main()

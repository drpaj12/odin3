"""Tests for tools/vtr-models (spec 3E §15.2, VB-16): extract over a tiny arch with a fake VTR
Python, check, check-lib, the committed models JSONs against spec §2's table, and the VPR negative
corpus tests/vtr/blif-bad (BlifBadCorpusTest, also CTest blif_vtr_check_bad).
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import vtr_models

from . import helpers

MINI_ARCH = helpers.FIXTURES / "vtr_models" / "mini_arch.xml"
ARCH_DIR = helpers.REPO_ROOT / "tests" / "vtr" / "arch"
K6_JSON = ARCH_DIR / "k6_frac_N10_frac_chain_mem32K_40nm.models.json"
EARCH_JSON = ARCH_DIR / "EArch.models.json"
VTR_LIB = helpers.REPO_ROOT / "lib" / "vtr.o3lib"
VTR_ROOT = vtr_models.DEFAULT_VTR_ROOT
VTR_ARCH_DIR = VTR_ROOT / "vtr_flow" / "arch" / "timing"
BAD_DIR = helpers.REPO_ROOT / "tests" / "vtr" / "blif-bad"
CHECKER = helpers.REPO_ROOT / "tools" / "blif-vtr-check" / "blif-vtr-check"
GIT = shutil.which("git")


def fake_vtr_root(root: Path, printed: str) -> Path:
    """A VTR checkout whose .venv/bin/python prints `printed`, as a git repo with one commit."""
    python = root / ".venv" / "bin" / "python"
    python.parent.mkdir(parents=True)
    python.write_text(f"#!/bin/sh\nprintf '{printed}'\n", encoding="utf-8")
    python.chmod(0o755)
    env = vtr_models._clean_env()
    git = [str(GIT), "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@example.com",
           "-c", "commit.gpgsign=false"]
    subprocess.run([*git, "init", "-q"], check=True, env=env)
    subprocess.run([*git, "commit", "-q", "--allow-empty", "-m", "fake"], check=True, env=env)
    return root


def write_lib(root: Path, text: str) -> Path:
    path = root / "t.o3lib"
    path.write_text("library t\n" + text, encoding="utf-8")
    return path


def k6_models() -> dict[str, object]:
    data = json.loads(K6_JSON.read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    models = data["models"]
    assert isinstance(models, dict)
    return models


@unittest.skipUnless(GIT, "git not found")
class ExtractTest(unittest.TestCase):
    def test_mini_arch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_vtr_root(Path(tmp), "6\\n15\\n")
            data = vtr_models.extract(MINI_ARCH, root)
        models = data["models"]
        assert isinstance(models, dict)
        self.assertEqual({"inputs": {"a": 1, "b": 1, "cin": 1},
                          "outputs": {"cout": 1, "sumout": 1}, "clocks": []}, models["adder"])
        # R11: the largest num_pins per port over the two RAM pb_types
        self.assertEqual({"inputs": {"we": 1, "addr": 15, "data": 64, "clk": 1},
                          "outputs": {"out": 64}, "clocks": ["clk"]}, models["single_port_ram"])
        self.assertEqual((1, "mini_arch.xml", 6, 15),
                         (data["schema"], data["arch"], data["lut_size"], data["ram_addr_width"]))
        self.assertRegex(str(data["vtr_commit"]), r"^[0-9a-f]{40}$")
        self.assertRegex(str(data["arch_sha256"]), r"^[0-9a-f]{64}$")
        self.assertEqual([], vtr_models.check_schema(data))

    def test_cli_out_is_sorted_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_vtr_root(Path(tmp) / "vtr", "6\\n15\\n")
            out = Path(tmp) / "m.json"
            code, _, err = helpers.run_main(vtr_models.main, [
                "extract", str(MINI_ARCH), "--out", str(out), "--vtr-root", str(root)])
            self.assertEqual((0, ""), (code, err))
            text = out.read_text(encoding="utf-8")
            self.assertEqual(vtr_models.dumps(json.loads(text)), text)
            code, _, err = helpers.run_main(vtr_models.main, ["check", str(out),
                                                              "--arch", str(MINI_ARCH)])
            self.assertEqual((0, ""), (code, err))

    def test_vtr_disagrees_on_lut_size(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_vtr_root(Path(tmp), "5\\n15\\n")
            code, out, err = helpers.run_main(vtr_models.main, [
                "extract", str(MINI_ARCH), "--vtr-root", str(root)])
        self.assertEqual((1, ""), (code, out))
        self.assertEqual("vtr-models: lut_size: VTR says 5, the pb_types give 6\n", err)

    def test_vtr_python_missing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            code, _, err = helpers.run_main(vtr_models.main, [
                "extract", str(MINI_ARCH), "--vtr-root", tmp])
        self.assertEqual(2, code)
        self.assertTrue(err.startswith("vtr-models: extract: VTR's Python: "), err)


class CheckTest(unittest.TestCase):
    def test_committed_jsons_pass(self) -> None:
        for path in (K6_JSON, EARCH_JSON):
            with self.subTest(path=path.name):
                code, out, err = helpers.run_main(vtr_models.main, ["check", str(path)])
                self.assertEqual((0, "", ""), (code, out, err))

    def test_missing_models(self) -> None:
        data = json.loads(K6_JSON.read_text(encoding="utf-8"))
        del data["models"]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.json"
            path.write_text(vtr_models.dumps(data), encoding="utf-8")
            code, _, err = helpers.run_main(vtr_models.main, ["check", str(path)])
        self.assertEqual((1, "vtr-models: check: models: missing\n"), (code, err))

    def test_schema_violations(self) -> None:
        data = json.loads(K6_JSON.read_text(encoding="utf-8"))
        data["schema"] = 2
        data["lut_size"] = True
        data["models"]["adder"]["clocks"] = ["clk"]
        data["models"]["multiply"]["inputs"]["a"] = 0
        self.assertEqual([
            "schema: not 1",
            "lut_size: not a positive integer",
            "models: adder: clocks: 'clk' is not an input",
            "models: multiply: inputs: port 'a' needs a positive width",
        ], vtr_models.check_schema(data))

    def test_not_json_is_exit_2(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.json"
            path.write_text("{", encoding="utf-8")
            code, _, err = helpers.run_main(vtr_models.main, ["check", str(path)])
        self.assertEqual(2, code)
        self.assertIn("not JSON", err)

    def test_stale_against_arch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            arch = Path(tmp) / "mini_arch.xml"
            arch.write_text(MINI_ARCH.read_text(encoding="utf-8").replace(
                'name="data" num_pins="64"', 'name="data" num_pins="32"'), encoding="utf-8")
            code, _, err = helpers.run_main(vtr_models.main, ["check", str(K6_JSON),
                                                              "--arch", str(arch)])
        self.assertEqual(1, code)
        self.assertIn("vtr-models: check: arch: 'k6_frac_N10_frac_chain_mem32K_40nm.xml' is not "
                      "'mini_arch.xml'\n", err)
        self.assertIn("vtr-models: check: arch_sha256: differs from mini_arch.xml", err)
        self.assertIn("vtr-models: check: models: single_port_ram: inputs: differs from the "
                      "arch\n", err)
        self.assertIn("vtr-models: check: models: multiply: not in the arch\n", err)

    @unittest.skipUnless(VTR_ARCH_DIR.is_dir(), "no VTR checkout")
    def test_committed_jsons_fresh(self) -> None:
        for path, arch in ((K6_JSON, "k6_frac_N10_frac_chain_mem32K_40nm.xml"),
                           (EARCH_JSON, "EArch.xml")):
            with self.subTest(arch=arch):
                self.assertEqual([], vtr_models.check(path, VTR_ARCH_DIR / arch))


class CommittedJsonTest(unittest.TestCase):
    """The committed k6 JSON equals spec §2's table; EArch has the same models."""

    def test_k6_equals_spec_table(self) -> None:
        data = json.loads(K6_JSON.read_text(encoding="utf-8"))
        self.assertEqual({
            "adder": {"inputs": {"a": 1, "b": 1, "cin": 1},
                      "outputs": {"cout": 1, "sumout": 1}, "clocks": []},
            "multiply": {"inputs": {"a": 36, "b": 36}, "outputs": {"out": 72}, "clocks": []},
            "single_port_ram": {"inputs": {"we": 1, "addr": 15, "data": 64, "clk": 1},
                                "outputs": {"out": 64}, "clocks": ["clk"]},
            "dual_port_ram": {"inputs": {"we1": 1, "we2": 1, "addr1": 15, "addr2": 15,
                                         "data1": 32, "data2": 32, "clk": 1},
                              "outputs": {"out1": 32, "out2": 32}, "clocks": ["clk"]},
        }, data["models"])
        self.assertEqual((1, 6, 15, "k6_frac_N10_frac_chain_mem32K_40nm.xml",
                          "3c9a4d23b27d187bd67c79dc01a18ff1ffc21d9b"),
                         (data["schema"], data["lut_size"], data["ram_addr_width"], data["arch"],
                          data["vtr_commit"]))

    def test_earch_same_models(self) -> None:
        k6 = json.loads(K6_JSON.read_text(encoding="utf-8"))
        earch = json.loads(EARCH_JSON.read_text(encoding="utf-8"))
        self.assertEqual(k6["models"], earch["models"])
        self.assertEqual("EArch.xml", earch["arch"])
        self.assertEqual((6, 15), (earch["lut_size"], earch["ram_addr_width"]))

    def test_committed_bytes_are_dumps(self) -> None:
        for path in (K6_JSON, EARCH_JSON):
            text = path.read_text(encoding="utf-8")
            self.assertEqual(vtr_models.dumps(json.loads(text)), text)


SPRAM = """\
cell single_port_ram hard
  param ADDR_WIDTH int 15 ; param DATA_WIDTH int 1
  in  clk 1{clock}
  in  data DATA_WIDTH
  in  addr ADDR_WIDTH
  in  we 1
  out out DATA_WIDTH
  memory words 2 ** ADDR_WIDTH ; width DATA_WIDTH
end
"""


class CheckLibTest(unittest.TestCase):
    def test_vtr_lib_against_k6(self) -> None:
        code, out, err = helpers.run_main(vtr_models.main, [
            "check-lib", str(VTR_LIB), "--models", str(K6_JSON)])
        self.assertEqual((0, "", ""), (code, out, err))
        self.assertEqual(0, helpers.run_main(vtr_models.main, ["check-lib", str(VTR_LIB)])[0])

    def test_extra_port(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            lib = write_lib(Path(tmp), "cell adder hard\n  in a 1 ; in b 1 ; in cin 1 ; in x 1\n"
                                       "  out cout 1 ; out sumout 1\nend\n")
            code, _, err = helpers.run_main(vtr_models.main, ["check-lib", str(lib)])
        self.assertEqual((1, "vtr-models: check-lib: adder: port 'x' is not on the model\n"),
                         (code, err))

    def test_clock_flag(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            good = write_lib(Path(tmp), SPRAM.format(clock=" clock"))
            self.assertEqual([], vtr_models.check_lib(good, k6_models()))
            lib = write_lib(Path(tmp), SPRAM.format(clock=""))
            code, _, err = helpers.run_main(vtr_models.main, ["check-lib", str(lib)])
        self.assertEqual((1, "vtr-models: check-lib: single_port_ram: port 'clk' clock flag "
                             "differs from the model\n"), (code, err))

    def test_other_reasons(self) -> None:
        text = (
            "cell mystery hard\n  in a 1\nend\n"
            "cell AND2 gate area 1\n  in A 1 ; in B 1 ; out Y 1\n  fn Y = A & B\nend\n"
            "cell adder hard\n  out a 1 ; in b 1 ; in cin 1 ; out cout 1 ; out sumout 1\nend\n"
            "cell multiply hard\n  param A_WIDTH int 40\n  param B_WIDTH int 36\n"
            "  in a A_WIDTH\n  out out A_WIDTH + B_WIDTH  # 76\nend\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            found = vtr_models.check_lib(write_lib(Path(tmp), text), k6_models())
        self.assertEqual([
            "mystery: no model of this name",
            "adder: port 'a' direction differs from the model",
            "multiply: port 'a' default width 40 exceeds the model's 36",
            "multiply: port 'out' default width 76 exceeds the model's 72",
            "multiply: model port 'b' is not on the cell",
        ], found)

    def test_lib_errors_are_exit_2(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            lib = write_lib(Path(tmp), "in a 1\n")
            code, _, err = helpers.run_main(vtr_models.main, ["check-lib", str(lib)])
            self.assertEqual(2, code)
            self.assertIn(":2: 'in' outside a cell", err)
            code, _, err = helpers.run_main(vtr_models.main, ["check-lib", str(Path(tmp) / "no")])
            self.assertEqual(2, code)

    def test_eval_width(self) -> None:
        params = {"A": 36, "N": 4}
        self.assertEqual(72, vtr_models.eval_width("A + A", params))
        self.assertEqual(16, vtr_models.eval_width("2 ** N", params))
        self.assertEqual(9, vtr_models.eval_width("(A >> 2)", params))
        self.assertIsNone(vtr_models.eval_width("B + 1", params))
        self.assertIsNone(vtr_models.eval_width("A ? 1 : 2", params))
        self.assertIsNone(vtr_models.eval_width("f(A)", params))


# --- the VPR negative corpus (CTest blif_vtr_check_bad) ------------------------------------------

# Each checker message, by rule class (spec §15.1's texts and Task 6's extra C3/C2 lines).
CLASSES = {
    "c1": r"illegal character <0x[0-9A-F]{2}> in name|unterminated quoted string",
    "c2": r"directive '.*' is not read by VPR|'.*' needs --circuit_format eblif",
    "c3": r"second model '.*' with primitives; VPR reads one|black-box model '.*' "
          r"(is not an arch model|has primitives)|stub port '.*' of '.*': .*"
          r"|no model with primitives; VPR needs one",
    "c4": r"\.names with \d+ inputs exceeds K=\d+",
    "c5": r"cover row has \d+ columns, expected \d+|cover row character '.*'",
    "c5b": r"1-input \.names '.*' has \d+ cover rows; VPR asserts exactly one "
           r"\(absorb_buffer_luts\)",
    "c6": r"latch type '.*' \(VPR accepts only re\)|latch without a clock|latch init '.*'"
          r"|'.*' on a \.latch line lexes as a keyword",
    "c7": r"\.subckt model '.*' is not an arch model|no port '.*' on model '.*'"
          r"|port '.*' exceeds width \d+ of '.*'|formal '.*' given twice",
    "c8": r"duplicate block name '.*' \(\w+ and \w+\)",
    "c9": r"net '.*' has \d+ drivers",
    "cs1": r"latch init 3 \(write 2; VTR's ABC rejects 3\)",
    "cs2": r"net 'unconn' feeds a \.names/\.latch pin \(VPR leaves it open\)",
    "cs3": r"'(gnd|vcc|unconn)' is not a constant generator of the right value",
    "cs4": r"missing header|\.clock present|stub '.*' unused",
    "cs5": r"clock net '.*' contains '\|' or '_\^_' \(breaks VTR's blackbox_latches\.pl\)",
}
TOP_FILES = {"c1_illegal_char", "c2_clock", "c3_two_models", "c3_bad_blackbox", "c4_wide_names",
             "c5_row_width", "c5b_buffer_rows", "c6_fe", "c6_no_clock", "c6_init4", "c6_keyword",
             "c7_no_model", "c7_no_port", "c7_wide_port", "c7_twice", "c8_dup_block",
             "c9_two_drivers"}
LINE = re.compile(r"^blif-vtr-check: (?P<path>.+):(?P<line>\d+): (?P<message>.+)$")


def run_checker(path: Path, strict: bool) -> tuple[int, list[str]]:
    """(exit code, the violation messages) of blif-vtr-check on path."""
    proc = subprocess.run([str(CHECKER), *(["--strict"] if strict else []), str(path)],
                          capture_output=True, text=True, check=False, timeout=60)
    messages = []
    for line in proc.stdout.splitlines():
        match = LINE.match(line)
        messages.append(match["message"] if match is not None else line)
    return proc.returncode, messages + proc.stderr.splitlines()


def rule_class(path: Path) -> str:
    match = re.match(r"(c\d+b?|cs\d)_", path.name)
    assert match is not None, path.name
    return match[1]


class BlifBadCorpusTest(unittest.TestCase):
    def test_every_rule_has_its_file(self) -> None:
        self.assertEqual(TOP_FILES, {p.stem for p in BAD_DIR.glob("*.blif")})
        strict = {rule_class(p) for p in (BAD_DIR / "strict").glob("*.blif")}
        self.assertEqual({"cs1", "cs2", "cs3", "cs4", "cs5"}, strict)

    def test_top_files_one_violation_of_their_class(self) -> None:
        for path in sorted(BAD_DIR.glob("*.blif")):
            with self.subTest(file=path.name):
                first = path.read_text(encoding="utf-8").splitlines()[0]
                self.assertRegex(first, r"^# vpr: \S")
                code, messages = run_checker(path, strict=False)
                self.assertEqual(1, code, messages)
                self.assertEqual(1, len(messages), messages)
                self.assertRegex(messages[0], f"^({CLASSES[rule_class(path)]})$")

    def test_strict_files_fail_only_with_strict(self) -> None:
        for path in sorted((BAD_DIR / "strict").glob("*.blif")):
            with self.subTest(file=path.name):
                self.assertEqual((0, []), run_checker(path, strict=False))
                code, messages = run_checker(path, strict=True)
                self.assertEqual(1, code, messages)
                self.assertEqual(1, len(messages), messages)
                self.assertRegex(messages[0], f"^({CLASSES[rule_class(path)]})$")


if __name__ == "__main__":
    unittest.main()

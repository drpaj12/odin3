"""Tests for tools/blif-vtr-check/blif_vtr_check.py (spec 3E §15.1, VB-15).

Each case writes an inline BLIF text to a temporary file and runs ``main`` on it, asserting the
exact violation lines (``<line>: <message>`` after the ``blif-vtr-check: <file>:`` prefix) and
the exit code. The models file is spec §2's table of the k6_frac arch, written here: the
committed ``tests/vtr/arch/*.models.json`` lands with ``tools/vtr-models`` (3E Task 7).
"""

from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import blif_vtr_check as bvc

from . import helpers

K6_MODELS = {
    "schema": 1,
    "lut_size": 6,
    "ram_addr_width": 15,
    "models": {
        "adder": {"inputs": {"a": 1, "b": 1, "cin": 1}, "outputs": {"cout": 1, "sumout": 1},
                  "clocks": []},
        "multiply": {"inputs": {"a": 36, "b": 36}, "outputs": {"out": 72}, "clocks": []},
        "single_port_ram": {"inputs": {"we": 1, "addr": 15, "data": 1, "clk": 1},
                            "outputs": {"out": 1}, "clocks": ["clk"]},
        "dual_port_ram": {"inputs": {"we1": 1, "we2": 1, "addr1": 15, "addr2": 15, "data1": 1,
                                     "data2": 1, "clk": 1},
                          "outputs": {"out1": 1, "out2": 1}, "clocks": ["clk"]},
    },
}
HEADER = "# Odin III 0.0.0 write_blif --vtr (K=6, names short)\n\n"
GOLDEN_BLIF = helpers.REPO_ROOT / "tests" / "golden" / "blif"
VTR_FIXTURES = helpers.REPO_ROOT / "tests" / "golden" / "blif-vtr"
# A Parmys golden read back keeps its `unconn` on logic pins (class U, spec §9.2); --strict
# flags that by design (C-S2), since VPR leaves those pins open until ABC folds them.
STRICT_C_S2 = {"shiftx.parmys.vtr.blif"}


def run_main(argv: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = bvc.main(argv)
    return code, out.getvalue(), err.getvalue()


class CheckerCase(unittest.TestCase):
    tmp: tempfile.TemporaryDirectory[str]
    models: Path

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.models = Path(cls.tmp.name) / "k6.models.json"
        cls.models.write_text(json.dumps(K6_MODELS), encoding="utf-8")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def check(self, text: str, *flags: str) -> tuple[int, list[str]]:
        path = Path(self.tmp.name) / "case.blif"
        path.write_bytes(text.encode("latin-1"))
        code, out, err = run_main(["--models", str(self.models), *flags, str(path)])
        self.assertEqual("", err)
        prefix = f"blif-vtr-check: {path}:"
        lines = out.splitlines()
        for line in lines:
            self.assertTrue(line.startswith(prefix), line)
        return code, [line[len(prefix):] for line in lines]

    def assert_violations(self, text: str, expected: list[str], *flags: str) -> None:
        code, lines = self.check(text, *flags)
        self.assertEqual(expected, lines)
        self.assertEqual(1 if expected else 0, code)


def model(body: str, inputs: str = "a", outputs: str = "y") -> str:
    """A top model: `.model t` (line 1), `.inputs` (2), `.outputs` (3), body from line 4."""
    return f".model t\n.inputs {inputs}\n.outputs {outputs}\n{body}.end\n"


ADDER_STUB = "\n.model adder\n.inputs a b cin\n.outputs cout sumout\n.blackbox\n.end\n"


class LexTest(unittest.TestCase):
    def kinds(self, text: str) -> list[tuple[str, str]]:
        return [(t.kind, t.text) for t in bvc.lex(text)]

    def test_hash_inside_a_word_is_a_name(self) -> None:
        self.assertEqual([("DIRECTIVE", ".inputs"), ("NAME", "a#b"), ("EOL", ""), ("EOL", "")],
                         self.kinds(".inputs a#b\n"))

    def test_hash_at_a_token_start_is_a_comment(self) -> None:
        self.assertEqual([("DIRECTIVE", ".inputs"), ("NAME", "a"), ("EOL", "#"), ("EOL", "")],
                         self.kinds(".inputs a #b c\n"))

    def test_continuation_in_every_state(self) -> None:
        text = ".names a \\\n b y\n1\\\n1 1\n.latch b \\\n c re a 2\n"
        self.assertEqual(
            [("DIRECTIVE", ".names"), ("NAME", "a"), ("NAME", "b"), ("NAME", "y"), ("EOL", ""),
             ("NAME", "1"), ("NAME", "1"), ("NAME", "1"), ("EOL", ""),
             ("DIRECTIVE", ".latch"), ("NAME", "b"), ("NAME", "c"), ("LATCH_KW", "re"),
             ("NAME", "a"), ("LATCH_KW", "2"), ("EOL", ""), ("EOL", "")],
            self.kinds(text))

    def test_latch_words_only_in_the_latch_state(self) -> None:
        tokens = list(bvc.lex(".inputs re NIL\n.latch re NIL fe rex 3\n"))
        self.assertEqual(["NAME", "NAME"], [t.kind for t in tokens[1:3]])
        self.assertEqual(["LATCH_KW", "LATCH_KW", "LATCH_KW", "NAME", "LATCH_KW"],
                         [t.kind for t in tokens[5:10]])
        self.assertEqual(2, tokens[5].line)

    def test_quoted_string_and_eq(self) -> None:
        self.assertEqual([("DIRECTIVE", ".subckt"), ("NAME", "m"), ("NAME", "a"), ("EQ", "="),
                          ("STRING", '"x y"'), ("EOL", ""), ("EOL", "")],
                         self.kinds('.subckt m a="x y"\n'))

    def test_blank_lines_are_no_eol_outside_a_cover(self) -> None:
        self.assertEqual([("DIRECTIVE", ".end"), ("EOL", ""), ("EOL", "")],
                         self.kinds("\n  \n.end\n"))

    def test_initial_continuation_into_a_blank_line_ends_the_line(self) -> None:
        # \{ENDL}{WS}*{ENDL} (INITIAL only) returns EOL; without it the blank line would be
        # swallowed and .outputs would continue the .inputs line
        self.assertEqual([("DIRECTIVE", ".inputs"), ("NAME", "a"), ("EOL", ""),
                          ("DIRECTIVE", ".outputs"), ("NAME", "y"), ("EOL", ""), ("EOL", "")],
                         self.kinds(".inputs a \\\n  \n.outputs y\n"))

    def test_newline_cr_is_one_endl(self) -> None:
        tokens = list(bvc.lex(".inputs a\n\r.outputs y\n"))
        outputs = [(t.text, t.line) for t in tokens if t.text == ".outputs"]
        self.assertEqual([(".outputs", 2)], outputs)

    def test_comment_at_eof_without_newline_is_a_word(self) -> None:
        # <*>#.*{ENDL} needs the ENDL, so flex lexes '#x' at the end of the file as a word
        self.assertEqual([("DIRECTIVE", ".inputs"), ("NAME", "a"), ("NAME", "#x"), ("EOL", "")],
                         self.kinds(".inputs a #x"))

    def test_illegal_byte(self) -> None:
        with self.assertRaises(bvc.LexError) as ctx:
            list(bvc.lex(".model t\n.names a=b y\n"))
        self.assertEqual((2, "illegal character <0x3D> in name"),
                         (ctx.exception.line, ctx.exception.message))


class C1Test(CheckerCase):
    def test_eq_in_a_name(self) -> None:
        self.assert_violations(model(".names a=b y\n1 1\n"),
                               ["4: illegal character <0x3D> in name"])

    def test_eq_on_an_inputs_line(self) -> None:
        self.assert_violations(model("", inputs="a=b"), ["2: illegal character <0x3D> in name"])

    def test_backslash_before_a_blank(self) -> None:
        self.assert_violations(model("", inputs="a\\ b"), ["2: illegal character <0x5C> in name"])

    def test_unterminated_quoted_string(self) -> None:
        self.assert_violations('.model t\n.inputs "abc\n.end\n',
                               ["2: unterminated quoted string"])

    def test_comment_at_eof_without_newline(self) -> None:
        self.assert_violations(model("") + "#x", ["5: directive '#x' is not read by VPR"])

    def test_continuation_into_a_blank_line(self) -> None:
        self.assert_violations(".model t\n.inputs a \\\n\n.outputs y\n.names a y\n1 1\n.end\n", [])

    def test_mid_line_directive(self) -> None:
        self.assert_violations(".model t\n.inputs a .outputs y\n.names a y\n1 1\n.end\n",
                               ["2: syntax: '.outputs' must start a line"])

    def test_lexer_error_ends_the_file(self) -> None:
        # only the lexer's line: no "no model" and no C5b for the cover cut short
        self.assert_violations('\n\n"abc\n', ["3: unterminated quoted string"])
        self.assert_violations(model(".names a y\n\x01 1\n"), ["5: cover row character '<0x01>'"])

    def test_hash_mid_name_accepted(self) -> None:
        self.assert_violations(model(".names a#b y\n1 1\n", inputs="a#b"), [])


class C2Test(CheckerCase):
    def test_clock(self) -> None:
        self.assert_violations(".model t\n.inputs clk\n.clock clk\n.end\n",
                               ["3: directive '.clock' is not read by VPR"])

    def test_clock_after_a_cover(self) -> None:
        # the line after a cover lexes in SO_COVER; '.clock clk' still gets C2, not 'c'
        self.assert_violations(model(".names a y\n1 1\n.clock clk\n"),
                               ["6: directive '.clock' is not read by VPR"])

    def test_attr_needs_eblif(self) -> None:
        text = model(".names a y\n1 1\n.attr x 1\n")
        self.assert_violations(text, ["6: '.attr' needs --circuit_format eblif"])
        self.assert_violations(text, [], "--eblif")


class C3Test(CheckerCase):
    def test_second_model_with_primitives(self) -> None:
        text = (model(".names a y\n1 1\n")
                + ".model m2\n.inputs b\n.outputs z\n.names b z\n1 1\n.end\n")
        self.assert_violations(text, ["7: second model 'm2' with primitives; VPR reads one"])

    def test_black_box_not_an_arch_model(self) -> None:
        text = model(".names a y\n1 1\n") + ".model foo\n.inputs p\n.blackbox\n.end\n"
        self.assert_violations(text, ["9: black-box model 'foo' is not an arch model"])

    def test_stub_port_too_wide(self) -> None:
        text = (model(".subckt adder a=a b=a cin=a cout=c sumout=y\n")
                + ".model adder\n.inputs a[0] a[1] b cin\n.outputs cout sumout\n.blackbox\n.end\n")
        self.assert_violations(text, ["7: stub port 'a' of 'adder': width 2 exceeds 1"])

    def test_stub_with_primitives(self) -> None:
        text = (model(".subckt adder a=a b=a cin=a cout=c sumout=y\n")
                + ".model adder\n.inputs a b cin\n.outputs cout sumout\n.names a cout\n1 1\n"
                ".blackbox\n.end\n")
        self.assert_violations(text, ["11: black-box model 'adder' has primitives"])

    def test_stub_port_direction_and_name(self) -> None:
        text = (model(".subckt adder a=a b=a cin=a cout=c sumout=y\n")
                + ".model adder\n.inputs a b cin cout x\n.outputs sumout\n.blackbox\n.end\n")
        self.assert_violations(text, [
            "7: stub port 'cout' of 'adder': direction differs from the model",
            "7: stub port 'x' of 'adder': no such port on the model"])

    def test_no_top_model(self) -> None:
        self.assert_violations(ADDER_STUB.lstrip("\n"),
                               ["1: no model with primitives; VPR needs one"])


class C4Test(CheckerCase):
    TEXT = model(".names a b c d e f g y\n1111111 1\n", inputs="a b c d e f g")

    def test_wide_names(self) -> None:
        self.assert_violations(self.TEXT, ["4: .names with 7 inputs exceeds K=6"])

    def test_lut_k_option(self) -> None:
        self.assert_violations(self.TEXT, [], "--lut-k", "7")


class C5Test(CheckerCase):
    def test_row_width(self) -> None:
        self.assert_violations(model(".names a y\n1 1 1\n"),
                               ["5: cover row has 2 columns, expected 1"])

    def test_row_character(self) -> None:
        self.assert_violations(model(".names a y\n2 1\n"), ["5: cover row character '2'"])

    def test_buffer_two_rows(self) -> None:
        self.assert_violations(
            model(".names a y\n1 1\n1 1\n"),
            ["4: 1-input .names 'y' has 2 cover rows; VPR asserts exactly one "
             "(absorb_buffer_luts)"])

    def test_buffer_no_rows(self) -> None:
        self.assert_violations(
            model(".names a y\n"),
            ["4: 1-input .names 'y' has 0 cover rows; VPR asserts exactly one "
             "(absorb_buffer_luts)"])

    def test_blank_line_ends_the_cover(self) -> None:
        self.assert_violations(
            model(".names a b y\n11 1\n\n00 1\n", inputs="a b"),
            ["7: syntax: cover row outside a .names (a blank or comment line ends the cover)"])

    def test_comment_on_the_names_line(self) -> None:
        self.assert_violations(
            model(".names a y # buffer\n1 1\n"),
            ["5: syntax: cover row after a comment on its .names line (VPR reads the row as "
             "names)"])
        self.assert_violations(model(".names gnd # zero\n.names a y\n1 1\n"), [])


class C6Test(CheckerCase):
    def latch(self, line: str, expected: list[str]) -> None:
        self.assert_violations(model(line + "\n", inputs="d c re", outputs="q"), expected)

    def test_falling_edge(self) -> None:
        self.latch(".latch d q fe c 0", ["4: latch type 'fe' (VPR accepts only re)"])

    def test_no_clock(self) -> None:
        for line in (".latch d q re NIL 0", ".latch d q", ".latch d q 2"):
            with self.subTest(line=line):
                self.latch(line, ["4: latch without a clock"])

    def test_bad_init(self) -> None:
        self.latch(".latch d q re c 4", ["4: latch init '4'"])
        self.latch(".latch d q 4", ["4: latch init '4'"])

    def test_keyword_as_a_net(self) -> None:
        self.latch(".latch re q re c 0", ["4: 're' on a .latch line lexes as a keyword"])

    def test_legal(self) -> None:
        self.latch(".latch d q re c 2", [])


class C7Test(CheckerCase):
    def test_not_an_arch_model(self) -> None:
        self.assert_violations(model(".subckt ram x=1\n"),
                               ["4: .subckt model 'ram' is not an arch model"])

    def test_no_port(self) -> None:
        self.assert_violations(model(".subckt adder q=x\n") + ADDER_STUB,
                               ["4: no port 'q' on model 'adder'"])

    def test_port_too_wide(self) -> None:
        text = model(".subckt single_port_ram addr[15]=x clk=a data=a we=a out=y\n")
        self.assert_violations(text, ["4: port 'addr[15]' exceeds width 15 of 'single_port_ram'"])

    def test_formal_twice(self) -> None:
        self.assert_violations(model(".subckt adder a=x a=y cout=y\n") + ADDER_STUB,
                               ["4: formal 'a' given twice"])
        # split_index: 'a' is bit 0 of a
        self.assert_violations(model(".subckt adder a=x a[0]=y cout=y\n") + ADDER_STUB,
                               ["4: formal 'a[0]' given twice"])


class C8C9Test(CheckerCase):
    def test_duplicate_block_name(self) -> None:
        text = model(".names a y\n1 1\n.subckt adder a=a b=b cin=b cout=y sumout=s\n",
                     inputs="a b") + ADDER_STUB
        # the subckt is named after its first output formal, cout; y is also driven twice
        self.assert_violations(text, ["6: duplicate block name 'y' (names and subckt)",
                                      "6: net 'y' has 2 drivers"])

    def test_outpad_name(self) -> None:
        self.assert_violations(model(".names a out:y\n1 1\n.names a y\n1 1\n"),
                               ["4: duplicate block name 'out:y' (output and names)"])

    def test_input_pad_is_a_driver(self) -> None:
        self.assert_violations(model(".names b a\n1 1\n", inputs="a b"),
                               ["4: duplicate block name 'a' (input and names)",
                                "4: net 'a' has 2 drivers"])

    def test_second_output_formal(self) -> None:
        text = model(".subckt adder a=a b=a cin=a cout=c sumout=y\n.names a y\n1 1\n") + ADDER_STUB
        self.assert_violations(text, ["5: net 'y' has 2 drivers"])


GOOD = HEADER + (
    ".model t\n.inputs a c\n.outputs y q\n.names gnd\n.names vcc\n1\n"
    ".subckt adder a=a b=gnd cin=vcc cout=y sumout=s\n.latch a q re c 2\n.end\n"
) + ADDER_STUB


class StrictTest(CheckerCase):
    def strict(self, text: str, expected: list[str]) -> None:
        self.assert_violations(text, expected, "--strict")

    def test_good_file(self) -> None:
        self.strict(GOOD, [])

    def test_init_3(self) -> None:
        text = GOOD.replace("re c 2", "re c 3")
        self.assert_violations(text, [])
        self.strict(text, ["10: latch init 3 (write 2; VTR's ABC rejects 3)"])

    def test_unconn_on_logic(self) -> None:
        text = GOOD.replace(".end\n", ".names unconn\n.names unconn a z\n10 1\n.end\n", 1)
        self.strict(text, ["12: net 'unconn' feeds a .names/.latch pin (VPR leaves it open)"])

    def test_constant_generator_value(self) -> None:
        text = GOOD.replace(".names gnd\n", ".names gnd\n1\n")
        self.assert_violations(text, [])
        self.strict(text, ["6: 'gnd' is not a constant generator of the right value"])

    def test_missing_header(self) -> None:
        self.strict(GOOD[len(HEADER):], ["1: missing header"])

    def test_header_k(self) -> None:
        self.strict(GOOD.replace("K=6", "K=4"), ["1: missing header"])

    def test_clock_present(self) -> None:
        text = GOOD.replace(".outputs y q\n", ".outputs y q\n.clock c\n")
        self.strict(text, ["6: directive '.clock' is not read by VPR", "6: .clock present"])

    def test_stub_unused(self) -> None:
        text = GOOD.replace(".subckt adder a=a b=gnd cin=vcc cout=y sumout=s\n",
                            ".names a y\n1 1\n")
        self.strict(text, ["14: stub 'adder' unused"])

    def test_clock_after_a_cover(self) -> None:
        text = GOOD.replace(".names vcc\n1\n", ".names vcc\n1\n.clock c\n")
        self.strict(text, ["9: directive '.clock' is not read by VPR", "9: .clock present"])

    def test_lexer_error_skips_whole_file_rules(self) -> None:
        # gnd is used before the error and its generator follows it; the stub's .subckt too
        text = (HEADER + ADDER_STUB.lstrip("\n")
                + ".model t\n.inputs a\n.outputs y\n.names gnd a y\n01 1\n.names a z\n2 1\n"
                ".subckt adder a=a b=a cin=a cout=c sumout=s\n.names gnd\n.end\n")
        self.strict(text, ["14: cover row character '2'"])

    def test_clock_net_name(self) -> None:
        text = GOOD.replace(" c\n", " a|b\n", 1).replace("re c 2", "re a|b 2")
        self.assert_violations(text, [])
        self.strict(text, ["10: clock net 'a|b' contains '|' or '_^_' "
                           "(breaks VTR's blackbox_latches.pl)"])

    def test_hard_clock_net_name(self) -> None:
        text = (HEADER + ".model t\n.inputs a k_^_x\n.outputs y\n"
                ".subckt single_port_ram addr=a clk=k_^_x data=a out=y we=a\n.end\n"
                "\n.model single_port_ram\n.inputs addr clk data we\n.outputs out\n.blackbox\n"
                ".end\n")
        self.strict(text, ["6: clock net 'k_^_x' contains '|' or '_^_' "
                           "(breaks VTR's blackbox_latches.pl)"])


class MainTest(CheckerCase):
    def test_max_errors(self) -> None:
        text = model(".clock a\n.clock b\n.clock c\n.clock d\n.clock e\n")
        code, lines = self.check(text)
        self.assertEqual((1, 5), (code, len(lines)))
        code, lines = self.check(text, "--max-errors", "2")
        self.assertEqual((1, ["4: directive '.clock' is not read by VPR",
                              "5: directive '.clock' is not read by VPR"]), (code, lines))

    def test_missing_file(self) -> None:
        missing = str(Path(self.tmp.name) / "missing.blif")
        code, out, err = run_main(["--models", str(self.models), missing])
        self.assertEqual((2, ""), (code, out))
        self.assertEqual(f"blif-vtr-check: {missing}: cannot read: No such file or directory\n",
                         err)

    def test_unreadable_models(self) -> None:
        bad = Path(self.tmp.name) / "bad.json"
        bad.write_text('{"lut_size": 6}', encoding="utf-8")
        code, out, err = run_main(["--models", str(bad), str(GOLDEN_BLIF / "ff.parmys.blif")])
        self.assertEqual((2, ""), (code, out))
        self.assertEqual(f"blif-vtr-check: {bad}: cannot read models: models: missing\n", err)

    def test_boolean_port_width(self) -> None:
        bad = Path(self.tmp.name) / "bool.json"
        data = json.loads(json.dumps(K6_MODELS))
        data["models"]["adder"]["inputs"]["a"] = True
        bad.write_text(json.dumps(data), encoding="utf-8")
        code, out, err = run_main(["--models", str(bad), str(GOLDEN_BLIF / "ff.parmys.blif")])
        self.assertEqual((2, ""), (code, out))
        self.assertEqual(f"blif-vtr-check: {bad}: cannot read models: models: adder: inputs: "
                         "port 'a' needs a positive width\n", err)

    def test_usage(self) -> None:
        code, _, err = run_main(["--max-errors", "0", "x.blif"])
        self.assertEqual(2, code)
        self.assertIn("expects a positive integer, got '0'", err)

    def test_load_models(self) -> None:
        models = bvc.load_models(self.models)
        self.assertEqual(6, models.lut_size)
        self.assertEqual(("clk",), models.models["single_port_ram"].clocks)
        self.assertEqual({"out": 72}, models.models["multiply"].outputs)


class GoldenTest(CheckerCase):
    def test_parmys_goldens_legal(self) -> None:
        paths = sorted(GOLDEN_BLIF.glob("*.parmys.blif"))
        self.assertGreaterEqual(len(paths), 5)
        for path in paths:
            with self.subTest(golden=path.name):
                code, out, err = run_main(["--models", str(self.models), str(path)])
                refused = path.name in ("dffsre.parmys.blif", "pow.parmys.blif")
                self.assertEqual((1 if refused else 0, ""), (code, err), out)
                if refused:  # $_DFFSR_PPP_ and $pow are no arch models
                    self.assertIn("is not an arch model", out)

    def test_vtr_fixtures_strict(self) -> None:
        # Every expected `write_blif --vtr` output passes --strict but the class-U golden.
        paths = sorted(VTR_FIXTURES.glob("*.vtr.blif"))
        self.assertGreaterEqual(len(paths), 10)
        for path in paths:
            with self.subTest(fixture=path.name):
                code, out, _ = run_main(["--models", str(self.models), "--strict", str(path)])
                if path.name in STRICT_C_S2:
                    self.assertEqual(1, code)
                    self.assertTrue(all(line.endswith("feeds a .names/.latch pin (VPR leaves it "
                                                      "open)") for line in out.splitlines()))
                else:
                    self.assertEqual((0, ""), (code, out))


if __name__ == "__main__":
    unittest.main()

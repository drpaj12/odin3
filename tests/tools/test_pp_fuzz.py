"""Tests for tools/pp-fuzz (spec 2B §10.5): the generator on fixed seeds, the reference expander
on the spec's own examples (text and where every byte is spelled), and the checks that compare
odin3's streams with it. CTest `pp_fuzz` runs the tool over the real odin3."""

from __future__ import annotations

import json
import os
import random
import re
import tempfile
import unittest
from pathlib import Path

import pp_fuzz as pf

from . import helpers

_TREES = tempfile.TemporaryDirectory(prefix="pp-fuzz-test-")


def tearDownModule() -> None:
    _TREES.cleanup()


def run_ref(
    files: dict[str, str],
    root: str = "src/t.v",
    defines: list[tuple[str, str | None]] | None = None,
) -> tuple[pf.Text, str]:
    """The reference's stream for root in a fresh tree holding files; (stream, tree)."""
    tmp = tempfile.mkdtemp(dir=_TREES.name)
    for rel, text in files.items():
        Path(tmp, rel).parent.mkdir(parents=True, exist_ok=True)
        Path(tmp, rel).write_bytes(text.encode("latin-1"))
    ref = pf.Reference(defines or [], [os.path.join(tmp, "inc"), os.path.join(tmp, "inc2")])
    return ref.stream(os.path.join(tmp, root)), tmp


def text(s: pf.Text) -> str:
    return "".join(c for c, _ in s)


def at(tree: str, rel: str, line: int, col: int) -> pf.Origin:
    """The origin of rel's line:col (1-based) in tree."""
    path = os.path.join(tree, rel)
    lines = Path(path).read_bytes().decode("latin-1").split("\n")
    return (path, sum(len(x) + 1 for x in lines[: line - 1]) + col - 1)


def definitions(text: str) -> list[tuple[str, list[str]]]:
    """(name, macros used) per `define of a generated macro, in order: strings and comments
    removed, continuations joined."""
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text).replace("\\\r\n", " ").replace("\\\n", " ")
    out = []
    for m in re.finditer(r"`define (M[0-9]+)([^\n]*)", text):
        out.append((m.group(1), re.findall(r"`(M[0-9]+)", m.group(2))))
    return out


class GeneratorTest(unittest.TestCase):
    def test_same_seed_same_project(self) -> None:
        self.assertEqual(pf.generate(1, 7), pf.generate(1, 7))
        self.assertNotEqual(pf.generate(1, 7), pf.generate(2, 7))
        self.assertNotEqual(pf.generate(1, 7), pf.generate(1, 8))

    def test_project_shape(self) -> None:
        proj = pf.generate(3, 0)
        o3proj = proj.o3proj()
        self.assertIn("incdir inc\nincdir inc2\n", o3proj)
        self.assertIn("define P0", o3proj)
        self.assertTrue(proj.roots and all(f"file verilog {r}" in o3proj for r in proj.roots))
        self.assertTrue(all(r in proj.files for r in proj.roots))

    def test_limits_hold(self) -> None:
        """Up to four formals, macro levels below the cap and calling only lower levels (so
        nesting stays within six with two levels of actuals), conditionals at most four deep."""
        self.assertLessEqual(pf.MAX_LEVEL + 2, 6)
        for case in range(60):
            gen = pf.Gen(random.Random(f"pp-fuzz/5/{case}"))
            for m in gen.macros:
                self.assertLessEqual(len(m.formals or []), 4)
                self.assertLessEqual(m.level, pf.MAX_LEVEL)
                if m.tail is not None:
                    self.assertLess(gen.macros[m.tail[0]].level, m.level)
            for rel, text in gen.project().files.items():
                body = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
                depth = most = 0
                for word in body.split("`")[1:]:
                    depth += word.startswith(("ifdef", "ifndef")) - word.startswith("endif")
                    most = max(most, depth)
                self.assertLessEqual(most, pf.MAX_COND, rel)

    def test_reference_accepts_every_generated_project(self) -> None:
        for case in range(150):
            proj = pf.generate(9, case)
            with tempfile.TemporaryDirectory() as tmp:
                root = Path(os.path.abspath(tmp))
                pf.write_project(proj, root)
                ref = pf.Reference(proj.defines, [str(root / d) for d in proj.incdirs])
                for r in proj.roots:
                    ref.stream(str(root / r))  # raises RefError on a generator bug
                self.assertEqual(os.path.normpath(root / proj.roots[0]), ref.files_read[0])

    def test_macro_nesting_is_bounded(self) -> None:
        """Every definition (bodies and defaults, redefinitions included) uses only macros of a
        lower level, so expansion through bodies nests at most MAX_LEVEL deep; file actuals add
        at most two levels (Gen.actual's nest < 2): six in all (§10.5)."""
        for case in range(80):
            proj = pf.generate(5, case)
            defs = definitions("".join(proj.files.values()))
            depth: dict[str, int] = {}
            for _ in defs:  # a fixpoint: the files are not in definition order
                for name, refs in defs:
                    used = max([depth.get(r, 1) for r in refs] or [0])
                    depth[name] = max(depth.get(name, 0), 1 + used)
            self.assertLessEqual(max(depth.values(), default=0), pf.MAX_LEVEL, case)

    def test_generator_covers_the_features(self) -> None:
        seen: set[str] = set()
        for case in range(80):
            proj = pf.generate(1, case)
            body = "".join(proj.files.values())
            gen = pf.Gen(random.Random(f"pp-fuzz/1/{case}"))
            defines = "\n".join(line for line in body.splitlines() if line.startswith("`define M"))
            uses = "\n".join(line for line in body.splitlines() if not line.startswith("`define"))
            seen |= {
                k
                for k, hit in {
                    "default": any(m.formals and any(m.formals) for m in gen.macros),
                    "comment in default": "=/*c*/ 3" in body or "= 1 /* c */+ 2" in body,
                    "string in body": '"' in defines,
                    "comment in body": "/* c pa */" in defines,
                    "nested use in actual": re.search(r"[(,]\s*`M[0-9]", uses) is not None,
                    "escaped actual": ",)q +1" in body,
                    "include": "`include" in body,
                    "elsif": "`elsif" in body,
                    "translate_off": "translate_off" in body,
                    "crlf": "\r\n" in body,
                    "continuation": "\\\n" in body or "\\\r\n" in body,
                    "line": "`line" in body,
                    "pass-through": "`timescale" in body,
                    "empty list": "()" in body,
                    "tail": any(m.tail for m in gen.macros),
                    "open tail": any(m.tail and m.tail[1] for m in gen.macros),
                    "cond in actual": "+ (2, 3) `else" in body,
                    "escaped": "\\bus" in body,
                }.items()
                if hit
            }
        self.assertEqual(18, len(seen), seen)


class ReferenceTest(unittest.TestCase):
    def test_comment_in_default_is_dropped(self) -> None:
        src = "`define A(x=/*c*/ 3) [x]\n`A()\n`define B(x=1 /* c */+ 2 /**/) [x]\n`B()\n"
        out, _ = run_ref({"src/t.v": src})
        self.assertEqual("\n[3]\n\n[1  + 2]\n", text(out))
        self.assertIsNone(out[9][1])

    def test_escaped_identifier_in_an_actual_is_opaque(self) -> None:
        out, _ = run_ref({"src/t.v": "`define A(x) [x]\n`A(\\a,b )\n"})
        self.assertEqual("\n[\\a,b]\n", text(out))

    def test_mid_line_conditional(self) -> None:
        out, _ = run_ref({"src/t.v": "a `ifdef X b `endif c\n"})
        self.assertEqual("a  c\n", text(out))

    def test_glued_use_gets_no_protective_space(self) -> None:
        out, _ = run_ref({"src/t.v": "`define S _y\nx`S\n"})
        self.assertEqual("\nx_y\n", text(out))

    def test_protective_space_is_loc_0(self) -> None:
        out, _ = run_ref({"src/t.v": "a`ifdef X b`else(c)`endif\n"})
        self.assertEqual("a (c)\n", text(out))
        self.assertIsNone(out[1][1])

    def test_include_without_final_newline(self) -> None:
        out, _ = run_ref({"src/t.v": 'x`include "h.vh"y\n', "inc/h.vh": "endmodule"})
        self.assertEqual("x endmodule y\n", text(out))
        self.assertEqual([None, None], [out[1][1], out[11][1]])

    def test_2a_example_spelling(self) -> None:
        src = "`define INNER(p) (p + 1)\n`define OUTER(q) `INNER(q * 2)\nassign y = `OUTER(xx);\n"
        out, tree = run_ref({"src/t.v": src})
        self.assertEqual("\n\nassign y = (xx * 2 + 1);\n", text(out))
        f = "src/t.v"
        want = [
            at(tree, f, 1, 18),
            at(tree, f, 3, 19),
            at(tree, f, 3, 20),
            *[at(tree, f, 2, 26 + k) for k in range(4)],
            *[at(tree, f, 1, 20 + k) for k in range(5)],
            at(tree, f, 3, 22),
        ]
        self.assertEqual(want, [o for _, o in out[13:26]])

    def test_defaults_and_empty_actuals(self) -> None:
        out, _ = run_ref({"src/t.v": "`define D(a, b=7) a+b\n`D(1) `D(1,) `D(,2)\n"})
        self.assertEqual("\n1+7 1+7 +2\n", text(out))

    def test_empty_formal_list_takes_no_actual(self) -> None:
        out, _ = run_ref({"src/t.v": "`define F() 5\n`F()\n"})
        self.assertEqual("\n5\n", text(out))

    def test_comment_in_actual_is_one_loc_0_space(self) -> None:
        out, _ = run_ref({"src/t.v": "`define I(x) [x]\n`I(p/*c*/q)\n"})
        self.assertEqual("\n[p q]\n", text(out))
        self.assertIsNone(out[3][1])

    def test_formal_in_string_or_escaped_identifier_untouched(self) -> None:
        out, _ = run_ref({"src/t.v": '`define S(x) "x" \\x x\nq = `S(a);\n'})
        self.assertEqual('\nq = "x" \\x a;\n', text(out))

    def test_continuation_keeps_the_newline(self) -> None:
        out, _ = run_ref({"src/t.v": "`define M wire\\\nx;\n`M\n"})
        self.assertEqual("\nwire\nx;\n", text(out))

    def test_escaped_identifier_keeps_its_terminator(self) -> None:
        src = "`define E \\a \t\n`define F \\b // c\n`define G \\c\nq = `E+`F+`G+d;\n"
        out, _ = run_ref({"src/t.v": src})
        self.assertEqual("\n\n\nq = \\a +\\b +\\c+d;\n", text(out))

    def test_crlf_body_ends_before_the_cr(self) -> None:
        out, _ = run_ref({"src/t.v": "`define E \\a\r\nq = `E;\r\n"})
        self.assertEqual("\r\nq = \\a;\r\n", text(out))

    def test_actuals_from_the_enclosing_text(self) -> None:
        src = "`define CALL `ADD\n`define ADD(a,b) a+b\n`CALL(x,y)\n`define O `ADD(\n`O p, q)\n"
        out, _ = run_ref({"src/t.v": src})
        self.assertEqual("\n\nx+y\n\np+q\n", text(out))

    def test_conditionals_inside_file_actuals(self) -> None:
        out, _ = run_ref({"src/t.v": "`define ID(x) x\n`ID(1 `ifdef X + 2 `else + 3 `endif)\n"})
        self.assertEqual("\n1  + 3\n", text(out))

    def test_translate_off_region_is_removed(self) -> None:
        src = "a // synopsys translate_off\nb `UNDEF\n/* synthesis translate_on */\nc\n"
        out, _ = run_ref({"src/t.v": src})
        self.assertEqual("a \nc\n", text(out))

    def test_include_search_order(self) -> None:
        files = {
            "src/t.v": '`include "h.vh"\n',
            "src/h.vh": '1 `include "g.vh"\n',
            "inc/h.vh": "2\n",
            "inc/g.vh": "3\n",
            "inc2/g.vh": "4\n",
        }
        out, _ = run_ref(files)
        self.assertEqual("1 3\n\n\n", text(out))

    def test_project_define_spelled_in_the_command_line(self) -> None:
        ref = pf.Reference([("E", None), ("P", "8")], [])
        self.assertEqual("`define E\n`define P 8\n", ref.cmdline)
        self.assertEqual([("8", (pf.CMDLINE, 20))], ref.macros["P"].body)

    def test_errors_are_reported(self) -> None:
        for src in (
            "`UNDEF\n",
            "`define A(x) x\n`A\n",
            "`define B `ifdef X\n`B\n",
            "`ifdef X\n",
            "`endif\n",
            "`define F() 1\n`F(2)\n",
        ):
            with self.subTest(src=src), self.assertRaises(pf.RefError):
                run_ref({"src/t.v": src})


class CheckTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.src = os.path.join(self.tmp.name, "a.v")
        Path(self.src).write_text("xy\nab\n", encoding="latin-1")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def seg(self, offset: int, line: int | None, col: int | None) -> dict[str, object]:
        where = None if line is None else self.src
        return {
            "out_offset": offset,
            "file": where,
            "spell_file": where,
            "spell_line": line,
            "spell_col": col,
        }

    def origins(
        self, text: str, segs: list[dict[str, object]]
    ) -> tuple[list[pf.Origin], list[str]]:
        probs: list[str] = []
        return pf.stream_origins(text, segs, pf.Sources(""), probs), probs

    def test_good_map(self) -> None:
        got, probs = self.origins(
            "y a", [self.seg(0, 1, 2), self.seg(1, None, None), self.seg(2, 2, 1)]
        )
        self.assertEqual([], probs)
        self.assertEqual([(self.src, 1), None, (self.src, 3)], got)

    def test_wrong_byte(self) -> None:
        _, probs = self.origins("ab", [self.seg(0, 1, 1)])
        self.assertIn("is not the byte spelled at", probs[0])

    def test_loc_0_rules(self) -> None:
        _, probs = self.origins("  ", [self.seg(0, None, None)])
        self.assertIn("loc-0 segment at 0", probs[0])
        _, probs = self.origins("y", [self.seg(0, None, None)])
        self.assertIn("loc-0 segment at 0", probs[0])

    def test_a_loc_without_a_spelling_fails(self) -> None:
        """A non-zero loc whose spelling chase failed (spell_file null, file not) is not a
        loc-0 blank; nor is the reverse."""
        bad = {**self.seg(0, None, None), "file": self.src}
        _, probs = self.origins(" ", [bad])
        self.assertIn("a loc without a spelling", probs[0])
        bad = {**self.seg(0, 1, 1), "file": None}
        _, probs = self.origins("x", [bad])
        self.assertIn("a loc without a spelling", probs[0])

    def test_segments_must_start_at_0_and_increase(self) -> None:
        _, probs = self.origins("xy", [self.seg(1, 1, 2)])
        self.assertIn("start at 0", probs[0])

    def test_tokens_and_their_spelling(self) -> None:
        want: pf.Text = [
            ("x", ("f", 0)),
            ("y", ("f", 1)),
            (" ", None),
            ("/", ("g", 0)),
            ("/", ("g", 1)),
            ("c", ("g", 2)),
        ]
        self.assertEqual(
            [("xy", (("f", 0), ("f", 1))), ("//c", (("g", 0), ("g", 1), ("g", 2)))], pf.tokens(want)
        )
        src = pf.Sources("")
        self.assertEqual([], pf.compare_tokens(want, list(want), src))
        moved = [("x", ("f", 0)), ("y", ("f", 2)), *want[2:]]
        probe = pf.compare_tokens(want, moved, pf.Sources(""))
        self.assertEqual(1, len(probe))
        self.assertIn("byte 1", probe[0])
        self.assertIn("expected 2 tokens, got 1", pf.compare_tokens(want, want[:2], src)[0])

    def test_check_stream_reads_the_outputs(self) -> None:
        out = Path(self.tmp.name)
        (out / "0.v").write_text("x", encoding="latin-1")
        (out / "0.segments.json").write_text(json.dumps([self.seg(0, 1, 1)]), encoding="latin-1")
        self.assertEqual([], pf.check_stream(0, [("x", (self.src, 0))], out, pf.Sources("")))
        self.assertIn(
            "stream 0: token 0", pf.check_stream(0, [("z", (self.src, 0))], out, pf.Sources(""))[0]
        )


class SeedsTest(unittest.TestCase):
    def test_seeds_get_both_options_bytes_smallest_first(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for rel, data in (
                ("src/a.v", b"module a;"),
                ("src/b.vh", b"`x"),
                ("src/notes.txt", b"x"),
                ("micro/plain.v", b"module p;"),
                ("micro/tick.v", b"`define A"),
            ):
                Path(tmp, rel).parent.mkdir(parents=True, exist_ok=True)
                Path(tmp, rel).write_bytes(data)
            code, out, _ = helpers.run_main(
                pf.main,
                [
                    "seeds",
                    os.path.join(tmp, "out"),
                    os.path.join(tmp, "src"),
                    "--backtick",
                    os.path.join(tmp, "micro"),
                ],
            )
            self.assertEqual((0, "pp-fuzz: 6 seeds from 3 files"), (code, out[:29]))
            got = {p.name: p.read_bytes() for p in Path(tmp, "out").iterdir()}
            self.assertEqual(
                {
                    "0000_b.vh.00": b"\0`x",
                    "0000_b.vh.c3": b"\xc3`x",
                    "0001_tick.v.00": b"\0`define A",  # same size: by path
                    "0001_tick.v.c3": b"\xc3`define A",
                    "0002_a.v.00": b"\0module a;",
                    "0002_a.v.c3": b"\xc3module a;",
                },
                got,
            )


if __name__ == "__main__":
    unittest.main()

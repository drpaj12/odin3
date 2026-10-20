"""Tests for tools/verilog-gen (2C spec §7.5, plan Task 11): the generator's own claims.

Deterministic per seed; every name declared before use; the minimal-parenthesis printer and the
tree agree on 500 random expressions (an independent precedence parser written here, from
1364-2005 Table 5-4, reads the printed text back to a fully parenthesised form, compared with
the tree's); the four associativity cases of the brief; and, when ODIN3_PARSE_CHECK is set (by
CTest), 50 seeds through odin3's oracle-free round trip (parse, check, print, re-parse, equal).
Also the fuzz seed writer's alphabet and compare.py's helpers.
"""

from __future__ import annotations

import os
import random
import re
import subprocess
import tempfile
import unittest
from collections.abc import Iterator
from pathlib import Path

import compare
import fuzz_seeds
import verilog_gen as vg

PARSE_CHECK = os.environ.get("ODIN3_PARSE_CHECK")

# --- an independent reader of the printed expressions ----------------------------------------

# Table 5-4, highest first; every binary operator is left-associative, ?: right.
LEVELS = [
    ["**"],
    ["*", "/", "%"],
    ["+", "-"],
    ["<<", ">>", "<<<", ">>>"],
    ["<", "<=", ">", ">="],
    ["==", "!=", "===", "!=="],
    ["&"],
    ["^", "~^", "^~"],
    ["|"],
    ["&&"],
    ["||"],
]
BINDING = {op: len(LEVELS) - idx + 1 for idx, ops in enumerate(LEVELS) for op in ops}
UNARY = {"+", "-", "!", "~", "&", "~&", "|", "~|", "^", "~^", "^~"}
TOKEN = re.compile(
    r"\s+|/\*.*?\*/|(\(\*.*?\*\))|(===|!==|<<<|>>>|\*\*|~\^|\^~|~&|~\||==|!=|"
    r"<=|>=|<<|>>|&&|\|\||[-+*/%<>&|^~!?:()])|([A-Za-z_][A-Za-z0-9_]*|\d+)"
    r"|\\(\S+)",
    re.S,
)


def tokens(text: str) -> list[str]:
    out = []
    pos = 0
    while pos < len(text):
        match = TOKEN.match(text, pos)
        assert match is not None, f"cannot read {text[pos:]!r}"
        pos = match.end()
        word = match.group(2) or match.group(3) or match.group(4)  # an escaped name: its bytes
        if word:
            out.append(word)
    return out


class Reader:
    """Precedence climbing over the token list; returns the fully parenthesised text."""

    def __init__(self, toks: list[str]) -> None:
        self.toks = toks
        self.pos = 0

    def peek(self) -> str:
        return self.toks[self.pos] if self.pos < len(self.toks) else ""

    def take(self) -> str:
        self.pos += 1
        return self.toks[self.pos - 1]

    def primary(self) -> str:
        tok = self.take()
        if tok == "(":
            inner = self.expression()
            assert self.take() == ")"
            return inner
        if tok in UNARY:  # a unary operator applies to a primary only (2C §3.3)
            op = "~^" if tok == "^~" else tok
            return f"({op}{self.primary()})"
        return tok

    def binary(self, level: int) -> str:
        left = self.primary()
        while BINDING.get(self.peek(), 0) >= level:
            op = self.take()
            right = self.binary(BINDING[op] + 1)
            left = f"({left} {'~^' if op == '^~' else op} {right})"
        return left

    def expression(self) -> str:
        cond = self.binary(1)
        if self.peek() != "?":
            return cond
        self.take()
        then = self.expression()
        assert self.take() == ":"
        other = self.expression()
        return f"({cond} ? {then} : {other})"


def full(node: vg.Node) -> str:
    """The tree's fully parenthesised text."""
    if node.kind == "IDENT":
        return node.name or ""
    if node.kind == "NUMBER":
        return str(node.spell["text"])
    if node.kind == "UNARY":
        return f"({node.sub}{full(node.kid(0))})"
    if node.kind == "BINARY":
        return f"({full(node.kid(0))} {node.sub} {full(node.kid(1))})"
    return f"({full(node.kid(0))} ? {full(node.kid(1))} : {full(node.kid(2))})"


def random_expr(rng: random.Random, depth: int) -> vg.Node:
    roll = rng.random()
    if depth <= 0 or roll < 0.2:
        return vg.ident(rng.choice("abcde")) if rng.random() < 0.8 else vg.num(rng.randint(0, 9))
    if roll < 0.35:
        node = vg.Node("UNARY", random_expr(rng, depth - 1), sub=rng.choice(vg.UNARY_OPS))
    elif roll < 0.85:
        node = vg.Node(
            "BINARY",
            random_expr(rng, depth - 1),
            random_expr(rng, depth - 1),
            sub=rng.choice(list(vg.BINARY_PREC)),
        )
    else:
        node = vg.Node("TERNARY", *(random_expr(rng, depth - 1) for _ in range(3)))
    if node.sub == "~^" and rng.random() < 0.3:
        node.spell["op"] = "^~"
    return node


def printed(node: vg.Node, seed: int = 0) -> str:
    printer = vg.Printer(random.Random(seed))
    printer.expr(node)
    return printer.out.text()


def parse(text: str) -> vg.Node:
    """A tiny tree builder for the brief's cases: a - (b - c) etc. (identifiers only)."""
    reader = Reader(tokens(text))
    return build(reader.expression())


def build(text: str) -> vg.Node:
    text = text.strip()
    if not text.startswith("("):
        return vg.ident(text)
    body = text[1:-1]
    depth = 0
    for idx, ch in enumerate(body):
        depth += ch == "("
        depth -= ch == ")"
        if depth == 0 and ch == " ":
            op_end = body.index(" ", idx + 1)
            return vg.Node(
                "BINARY", build(body[:idx]), build(body[op_end + 1 :]), sub=body[idx + 1 : op_end]
            )
    raise AssertionError(text)


# --- the generator's tree walks ----------------------------------------------------------------

DECLARING = {"DECLARATOR", "PORT_REF", "FUNCTION_DECL", "TASK_DECL", "INSTANCE"}
SCOPES = {"FUNCTION_DECL", "TASK_DECL", "SEQ_BLOCK", "GEN_BLOCK"}


def undeclared(node: vg.Node, scopes: list[set[str]], modules: set[str]) -> Iterator[str]:
    """Names used before their declaration, walking in source order."""
    if node.kind in DECLARING and node.name:
        scopes[-1].add(node.name)
    if node.kind in SCOPES:
        scopes = scopes + [set()]
        if node.kind in ("FUNCTION_DECL", "TASK_DECL") and node.name:
            scopes[-1].add(node.name)
    uses = node.kind in ("IDENT", "CALL", "TASK_CALL") and not (node.name or "").startswith("$")
    if uses and not any(node.name in scope for scope in scopes):
        yield f"{node.kind} {node.name} at {node.loc}"
    if node.kind == "INSTANTIATION" and node.name not in modules:
        yield f"module {node.name} at {node.loc}"
    if node.kind == "HIER_NAME":  # defparam u.P: u is an instance above, P its module's
        yield from undeclared(node.kid(0), scopes, modules)
        return
    for kid in node.kids:
        if kid is not None:
            yield from undeclared(kid, scopes, modules)


class GeneratorTest(unittest.TestCase):
    def test_deterministic_per_seed(self) -> None:
        for seed in (1, 17, 4242):
            first, again = vg.render(seed, "f.v"), vg.render(seed, "f.v")
            self.assertEqual(first.text, again.text)
            self.assertEqual(first.ast, again.ast)
        self.assertNotEqual(vg.render(1, "f.v").text, vg.render(2, "f.v").text)
        self.assertEqual(vg.render(5, "f.v", True).text, vg.render(5, "f.v", True).text)

    def test_every_name_declared_before_use(self) -> None:
        for seed in range(1, 201):
            modules: set[str] = set()
            for item in vg.generate(seed):
                vg.print_unit([item], random.Random(0))
                missing = list(undeclared(item, [set()], modules))
                self.assertEqual(missing, [], f"seed {seed}")
                if item.kind == "MODULE" and item.name:
                    modules.add(item.name)

    def test_minimal_parentheses_keep_the_tree(self) -> None:
        rng = random.Random(2026)
        for idx in range(500):
            node = random_expr(rng, rng.randint(1, 5))
            text = printed(node, idx)
            self.assertEqual(Reader(tokens(text)).expression(), full(node), text)

    def test_associativity_cases(self) -> None:
        cases = {
            "a - (b - c)": "a - (b - c)",
            "(a - b) - c": "a - b - c",
            "a ** (b ** c)": "a ** (b ** c)",
            "(a ** b) ** c": "a ** b ** c",
        }
        for source, expected in cases.items():
            self.assertEqual(printed(parse(source)), expected)

    def test_unary_operand_is_a_primary(self) -> None:
        inner = vg.Node("UNARY", vg.ident("a"), sub="-")
        self.assertEqual(printed(vg.Node("UNARY", inner, sub="-")), "-(-a)")
        power = vg.Node("BINARY", inner, vg.ident("b"), sub="**")
        self.assertEqual(printed(power), "-a ** b")
        self.assertEqual(printed(vg.Node("UNARY", power, sub="~")), "~(-a ** b)")

    def test_number_payloads(self) -> None:
        cases = {
            "8'hFF": "8'hff",
            "'sd5": "5",
            "4'b?z?Z": "4'b????",
            "8 'h F_f": "8'hff",
            "'h0_0000_0001": "'h000000001",
            "8'dZ": "8'dz",
            "16'd065_535": "16'd65535",
            "8'SB1111_0000": "8'sb11110000",
            "42": "42",
            "'d9": "'d9",
        }
        for spelling, payload in cases.items():
            self.assertEqual(vg.number_node(spelling).payload, payload, spelling)
        self.assertEqual(vg.real_node("1e3").payload, "1000.0")
        self.assertEqual(vg.real_node("3_0.25").payload, "30.25")

    def test_dump_shape(self) -> None:
        node = vg.Node("CASE_ITEM", vg.Node("NULL_STMT"), vg.num(1)).flag("DEFAULT")
        node.attrs.append(vg.attr("k", vg.string_node('"a\\tb"')))
        out: list[str] = []
        vg.dump(node, "f.v", out)
        self.assertEqual(
            "".join(out),
            "(CASE_ITEM [DEFAULT] @f.v:0:0\n  (NULL_STMT @f.v:0:0)\n"
            "  (NUMBER.dec [SIGNED] @f.v:0:0 1)\n  {(ATTR:k @f.v:0:0\n"
            '     (STRING:"a\\tb" @f.v:0:0))})',
        )

    def test_mutation_changes_one_token(self) -> None:
        for seed in range(1, 500):  # seed 406 once swapped ')' with ')': a no-op
            made = vg.render(seed, "f.v", mutated=True)
            self.assertNotEqual(made.text, vg.render(seed, "f.v").text)
            self.assertRegex(made.note, r"^(drop|duplicate|swap) ")

    @unittest.skipIf(not PARSE_CHECK, "ODIN3_PARSE_CHECK not set (run through CTest)")
    def test_fifty_seeds_round_trip_in_odin3(self) -> None:
        assert PARSE_CHECK is not None
        with tempfile.TemporaryDirectory() as tmp:
            names = []
            for seed in range(1, 51):
                path = Path(tmp) / f"gen_{seed}.v"
                path.write_text(vg.render(seed, path.name).text, encoding="utf-8")
                names.append(str(path))
            result = subprocess.run(
                [PARSE_CHECK, "--no-oracle", *names],
                capture_output=True,
                text=True,
                timeout=600,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr[-4000:])


class ToolsTest(unittest.TestCase):
    def test_fuzz_alphabet_has_64_spellings(self) -> None:
        spellings = fuzz_seeds.alphabet()
        self.assertEqual(len(spellings), 64)
        self.assertIn(b"\\c+d ", spellings)
        self.assertIn(b'"s\\n"', spellings)
        self.assertIn(b"// synopsys full_case parallel_case\n", spellings)
        stream = fuzz_seeds.token_bytes(b"module m; wire x; endmodule // c\n", spellings)
        self.assertEqual(
            [spellings[b].strip() for b in stream],
            [b"module", b"a", b";", b"wire", b"a", b";", b"endmodule", b"// c"],
        )

    def test_seeds_begin_with_a_bom_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "a.v"
            src.write_text("module a; endmodule\n", encoding="utf-8")
            out = Path(tmp) / "seeds"
            self.assertEqual(fuzz_seeds.write_seeds(out, [src], 1), 5)
            self.assertTrue((out / "seed_0000").read_bytes().startswith(b"\x00\xef\xbb\xbf"))
            self.assertTrue((out / "seed_0003").read_bytes().startswith(b"\x01"))

    def test_seeds_take_regression_inputs_as_they_are(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "a.v"
            src.write_text("module a; endmodule\n", encoding="utf-8")
            regress = Path(tmp) / "regress"
            regress.mkdir()
            (regress / "r.v").write_bytes(b"\x00`pragma a\x00b\n")
            out = Path(tmp) / "seeds"
            self.assertEqual(fuzz_seeds.write_seeds(out, [src], 0, regress), 5)
            self.assertEqual((out / "seed_0004").read_bytes(), b"\x00`pragma a\x00b\n")

    def test_compare_helpers(self) -> None:
        self.assertEqual(compare.parse_seeds("3..5"), range(3, 6))
        self.assertEqual(compare.parse_seeds("7"), range(7, 8))
        self.assertTrue(compare.located("odin3: error: g.v:2:16: error: syntax: x", "g.v"))
        self.assertFalse(compare.located("odin3: error: g.v:0:0: error: x", "g.v"))
        self.assertFalse(compare.located("odin3: error: pass read_verilog: failed", "g.v"))
        self.assertEqual(
            compare.crashed(compare.Run(1, "", "==1== ERROR: AddressSanitizer")), "sanitizer report"
        )
        self.assertEqual(compare.fatal_status("odin3: error: pass x: failed: ODIN3_ERR_PARSE"), "")
        self.assertEqual(
            compare.fatal_status(
                "g.v:1:1: error: x\nodin3: error: pass x: failed: ODIN3_ERR_INVALID_ARG"
            ),
            "ODIN3_ERR_INVALID_ARG",
        )
        self.assertEqual(compare.first_error("dump...\nFAIL g.v: x\n_)))", "g.v"), "FAIL g.v: x")

    def test_compare_resolves_relative_tools(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            tool = Path(tmp) / "bin" / "tool"
            tool.parent.mkdir()
            tool.write_text("#!/bin/sh\n", encoding="utf-8")
            tool.chmod(0o755)
            here = os.getcwd()
            try:
                os.chdir(tmp)
                given = compare.Tools("bin/tool", "bin/tool", "bin/missing")
                tools, missing = compare.resolve_tools(given)
            finally:
                os.chdir(here)
            self.assertEqual(tools.odin3, str(tool.resolve()))
            self.assertEqual(missing, ["bin/missing"])

    def test_compare_work_dir_drops_old_files(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "gen_7.v").write_text("old", encoding="utf-8")
            (Path(tmp) / "keep.txt").write_text("mine", encoding="utf-8")
            work = compare.work_dir(tmp)
            self.assertEqual(sorted(p.name for p in work.iterdir()), ["keep.txt"])


if __name__ == "__main__":
    unittest.main()

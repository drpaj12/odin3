"""Tests for tools/check_rules.py (spec §15.1 rules not covered by other tools)."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path
from types import ModuleType


def _load() -> ModuleType:
    path = Path(__file__).resolve().parents[2] / "tools" / "check_rules.py"
    spec = importlib.util.spec_from_file_location("check_rules", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rules = _load()


class CheckRulesTest(unittest.TestCase):
    def check(self, path: str, text: str) -> list[str]:
        result: list[str] = rules.violations(path, text)
        return result

    def test_clean_file_passes(self) -> None:
        text = "int f(void) {\n    int rc = 0;\n    goto cleanup;\ncleanup:\n    return rc;\n}\n"
        self.assertEqual(self.check("src/passes/opt.c", text), [])

    def test_exit_outside_cli_is_flagged(self) -> None:
        self.assertEqual(len(self.check("src/passes/opt.c", "void f(void) { exit(1); }\n")), 1)
        self.assertEqual(len(self.check("src/passes/opt.c", "void f(void) { _Exit(1); }\n")), 1)

    def test_exit_aliases_are_flagged(self) -> None:
        for text in (
            "void (*quit_fn)(int) = exit;\n",
            "#define QUIT exit\n",
            "void f(void) { _exit(1); }\n",
            "void f(void) { (exit)(1); }\n",
        ):
            with self.subTest(text=text):
                self.assertEqual(len(self.check("src/passes/opt.c", text)), 1)

    def test_identifiers_containing_exit_are_allowed(self) -> None:
        text = "int exit_code; int on_exit_hook; void f(void) { abort_count++; }\n"
        self.assertEqual(self.check("src/passes/opt.c", text), [])

    def test_exit_in_cli_is_allowed(self) -> None:
        self.assertEqual(self.check("src/cli/main.c", "void f(void) { exit(1); }\n"), [])

    def test_abort_only_in_check(self) -> None:
        text = "void f(void) { abort(); }\n"
        self.assertEqual(len(self.check("src/passes/opt.c", text)), 1)
        self.assertEqual(self.check("src/ir/check.c", text), [])

    def test_trap_and_sigabrt_only_in_check(self) -> None:
        for text in ("void f(void) { __builtin_trap(); }\n", "void f(void) { raise(SIGABRT); }\n"):
            with self.subTest(text=text):
                self.assertEqual(len(self.check("src/passes/opt.c", text)), 1)
                self.assertEqual(self.check("src/ir/check.c", text), [])

    def test_assert_and_other_signals_are_allowed(self) -> None:
        text = "void f(int n) { assert(n > 0); raise(SIGINT); }\n"
        self.assertEqual(self.check("src/passes/opt.c", text), [])

    def test_goto_other_label_is_flagged(self) -> None:
        found = self.check("src/util/vec.c", "void f(void) { goto out; out: ; }\n")
        self.assertEqual(found, ["src/util/vec.c:1: goto may only target the 'cleanup' label"])

    def test_comments_and_strings_are_ignored(self) -> None:
        text = '/* never exit() here */\n// abort() is banned\nconst char *s = "goto x";\n'
        self.assertEqual(self.check("src/passes/opt.c", text), [])

    def test_line_numbers_survive_block_comments(self) -> None:
        text = "/*\n\n*/\nvoid f(void) { abort(); }\n"
        expected = ["src/a.c:4: abort/trap is only allowed in src/ir/check*.c"]
        self.assertEqual(self.check("src/a.c", text), expected)


def _grammar(rules: str, prologue: str = "") -> str:
    """A .y file: an optional prologue block, one token, then the rules section."""
    return f"{prologue}%token N\n%%\n{rules}%%\n"


def _scanner(rules: str, prologue: str = "") -> str:
    """A .l file: options, an optional prologue block, then the rules section."""
    return f"%option reentrant\n{prologue}%%\n{rules}%%\n"


class GeneratedSourceRulesTest(unittest.TestCase):
    """The hygiene rules for .y/.l (2C spec §6): their actions and prologues compile inside the
    generated translation units, so they hold no hand-written logic."""

    Y = "src/frontends/verilog/verilog.y"
    L = "src/frontends/verilog/verilog.l"

    def check(self, path: str, text: str) -> list[str]:
        result: list[str] = rules.violations(path, text)
        return result

    def assert_rule(self, path: str, text: str, rule: str) -> None:
        found = self.check(path, text)
        self.assertEqual(len(found), 1, found)
        self.assertIn(f": {rule}: ", found[0])

    def test_one_vp_statement_passes(self) -> None:
        text = _grammar(
            "e: N { VP($$, vp_x(ctx, &$$)); }\n | e N { VP_ERR(vp_bad(ctx, &@2)); }\n ;\n"
        )
        self.assertEqual(self.check(self.Y, text), [])

    def test_rule_without_action_passes(self) -> None:
        self.assertEqual(self.check(self.Y, _grammar("e: %empty ;\n")), [])

    def test_plain_assignment_fails(self) -> None:
        self.assert_rule(self.Y, _grammar("e: N { $$ = $1; } ;\n"), "y-action")

    def test_two_vp_statements_fail(self) -> None:
        self.assert_rule(self.Y, _grammar("e: N { VP(a, f()); VP(b, g()); } ;\n"), "y-action")

    def test_four_line_action_fails(self) -> None:
        text = _grammar("e: N {\n  VP($$,\n     vp_x(ctx, &$$));\n} ;\n")
        self.assert_rule(self.Y, text, "action-lines")

    def test_three_line_action_passes(self) -> None:
        text = _grammar("e: N {\n  VP($$, vp_x(ctx, &$$)); }\n ;\n")
        self.assertEqual(self.check(self.Y, text), [])

    def test_braces_in_char_literals_and_comments_are_not_actions(self) -> None:
        text = _grammar("e: '{' N '}' /* { not an action } */ { VP($$, vp_x(ctx, &$$)); } ;\n")
        self.assertEqual(self.check(self.Y, text), [])

    def test_prologue_code_fails(self) -> None:
        self.assert_rule(self.Y, _grammar("e: N ;\n", "%{ int x; %}\n"), "prologue")
        self.assert_rule(
            self.L, _scanner("x { odin3_vl_x(yyextra); }\n", "%{\nint x;\n%}\n"), "prologue"
        )
        self.assert_rule(self.Y, _grammar("e: N ;\n", "%code { static int x; }\n"), "prologue")

    def test_prologue_includes_and_allowed_macros_pass(self) -> None:
        prologue = (
            '%{ #include "x.h" %}\n'
            '%code requires {\n#include "frontends/verilog/parser.h"\n}\n'
            "%code {\n"
            "#define YYLLOC_DEFAULT(Cur, Rhs, N) odin3_vp_lloc(&(Cur), (Rhs), (N))\n"
            "#define YYMAXDEPTH odin3_vp_yymaxdepth()\n"
            "#define YYINITDEPTH ODIN3_VP_YYINITDEPTH\n"
            "#define YYSTACK_USE_ALLOCA 0\n"
            "}\n"
        )
        self.assertEqual(self.check(self.Y, _grammar("e: N ;\n", prologue)), [])
        lexer = (
            '%{\n#include "x.h"\n'
            "#define YY_USER_ACTION odin3_vl_user_action(yyextra, yyleng);\n%}\n"
        )
        self.assertEqual(self.check(self.L, _scanner("x { odin3_vl_x(yyextra); }\n", lexer)), [])

    def test_other_macro_in_prologue_fails(self) -> None:
        self.assert_rule(self.Y, _grammar("e: N ;\n", "%{\n#define YYDEBUG 1\n%}\n"), "prologue")

    def test_epilogue_code_fails(self) -> None:
        self.assert_rule(
            self.Y, _grammar("e: N ;\n") + "int helper(void) { return 0; }\n", "prologue"
        )

    def test_lexer_action_one_call_passes(self) -> None:
        text = _scanner(
            "<<EOF>>  { return odin3_vl_eof(yyextra, yylloc); }\n"
            '"{"|[{]  { odin3_vl_unexpected(yyextra, yytext); }\n'
            "a{2,3}   |\n"
            "'[sS]?[bB] { BEGIN(BASED); return odin3_vl_base(yyextra, yylval, yylloc); }\n"
            "<BASED>. { odin3_vl_putback(yyextra, yyleng); yyless(0); BEGIN(INITIAL); }\n"
        )
        self.assertEqual(self.check(self.L, text), [])

    def test_lexer_action_other_code_fails(self) -> None:
        self.assert_rule(self.L, _scanner("x { yyextra->offset++; }\n"), "l-action")
        self.assert_rule(
            self.L, _scanner("x { odin3_vl_a(yyextra); odin3_vl_b(yyextra); }\n"), "l-action"
        )
        self.assert_rule(self.L, _scanner("x { return TOK_X; }\n"), "l-action")

    def test_four_line_lexer_action_fails(self) -> None:
        text = _scanner("x {\n    return odin3_vl_x(yyextra,\n        yylval);\n}\n")
        self.assert_rule(self.L, text, "action-lines")

    def test_goto_in_grammar_still_fails(self) -> None:
        found = self.check(self.Y, _grammar("e: N { VP($$, vp_x(ctx, &$$)); goto x; } ;\n"))
        self.assertIn(f"{self.Y}:3: goto may only target the 'cleanup' label", found)

    def test_c_files_are_not_held_to_action_rules(self) -> None:
        self.assertEqual(self.check("src/a.c", "int f(void) { int x = 1; return x; }\n"), [])


if __name__ == "__main__":
    unittest.main()

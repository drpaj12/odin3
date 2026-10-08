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

    def test_exit_in_cli_is_allowed(self) -> None:
        self.assertEqual(self.check("src/cli/main.c", "void f(void) { exit(1); }\n"), [])

    def test_abort_only_in_check(self) -> None:
        text = "void f(void) { abort(); }\n"
        self.assertEqual(len(self.check("src/passes/opt.c", text)), 1)
        self.assertEqual(self.check("src/ir/check.c", text), [])

    def test_goto_other_label_is_flagged(self) -> None:
        found = self.check("src/util/vec.c", "void f(void) { goto out; out: ; }\n")
        self.assertEqual(found, ["src/util/vec.c:1: goto may only target the 'cleanup' label"])

    def test_comments_and_strings_are_ignored(self) -> None:
        text = '/* never exit() here */\n// abort() is banned\nconst char *s = "goto x";\n'
        self.assertEqual(self.check("src/passes/opt.c", text), [])

    def test_line_numbers_survive_block_comments(self) -> None:
        text = "/*\n\n*/\nvoid f(void) { abort(); }\n"
        expected = ["src/a.c:4: abort() is only allowed in src/ir/check*.c"]
        self.assertEqual(self.check("src/a.c", text), expected)


if __name__ == "__main__":
    unittest.main()

"""Tests for tools/abc-check/abc-equiv (3D Task 19), with fakes for odin3 and equiv-check."""

from __future__ import annotations

import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import abc_equiv
import qor_heavy

from . import helpers

FAKE_ODIN3 = """#!{python}
import json, os, re, shutil, sys
argv = sys.argv
log = os.environ.get("FAKE_LOG")
with open(log + ".odin3", "a", encoding="utf-8") as fh:
    fh.write(json.dumps({{"argv": argv, "env": dict(os.environ)}}) + "\\n")
script = argv[2]
design = re.search(r"read_blif (\\S+);", script).group(1)
targets = re.findall(r"write_blif (?:--vtr )?(\\S+?)(?:;|$)", script)
name = os.path.basename(design)
if "views" in name:
    sys.stderr.write("abc: module 'pow' is not in the netlist view: $pow 'c'; run lower first\\n")
    sys.exit(1)
if "broken" in name:
    sys.stderr.write("abc: something else\\n")
    sys.exit(0)
text = open(design, encoding="utf-8").read()
for target in targets:
    out = text
    if "unconn" in name and "after" in os.path.basename(target):
        out = text.replace("addr[5]=unconn", "addr[5]=n5")
    with open(target, "w", encoding="utf-8") as fh:
        fh.write(out)
"""

FAKE_EQUIV = """#!{python}
import json, os, sys
argv = sys.argv
with open(os.environ["FAKE_LOG"] + ".equiv", "a", encoding="utf-8") as fh:
    fh.write(json.dumps({{"argv": argv, "env": dict(os.environ)}}) + "\\n")
text = open(argv[-2], encoding="utf-8").read()
if "# differs" in text:
    print("equiv-check: NOT equivalent: outputs differ")
    sys.exit(1)
if "# multiclock" in text:
    sys.stderr.write("equiv-check: unproven: x: unsupported: more than one clock domain\\n")
    sys.exit(4)
print("equiv-check: equivalent (abc cec)")
"""

SPR = ".model spr\n.inputs addr[5]\n.outputs out[0]\n.blackbox\n.end\n"


def design_text(extra: str = "", pad: int = 0) -> str:
    return (".model t\n.inputs a\n.outputs y\n.names a y\n1 1\n" + extra + ".end\n" + "#" * pad
            + "\n")


class AbcEquivTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.odin3 = self.script("odin3", FAKE_ODIN3)
        self.equiv = self.script("equiv-check", FAKE_EQUIV)
        self.log = str(self.root / "fake")
        patcher = mock.patch.dict(os.environ, {"FAKE_LOG": self.log})
        patcher.start()
        self.addCleanup(patcher.stop)
        self.keep = self.root / "keep"

    def script(self, name: str, text: str) -> str:
        path = self.root / name
        path.write_text(text.format(python=sys.executable), encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return str(path)

    def blif(self, rel: str, text: str) -> str:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return str(path)

    def calls(self, kind: str) -> list[dict[str, object]]:
        path = Path(self.log + "." + kind)
        if not path.exists():
            return []
        return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]

    def run_tool(self, *extra: str) -> tuple[int, str, str]:
        argv = ["--odin3", self.odin3, "--equiv-check", self.equiv, "--keep", str(self.keep),
                *extra]
        return helpers.run_main(abc_equiv.main, argv)

    def test_one_size_order_across_two_directories(self) -> None:
        big = self.blif("d1/a.blif", design_text(pad=40))
        small = self.blif("d1/c.blif", design_text())
        mid = self.blif("d2/b.blif", design_text(pad=10))
        code, out, err = self.run_tool(str(self.root / "d1"), str(self.root / "d2"))
        self.assertEqual(code, 0, out + err)
        lines = [ln.split()[0] for ln in out.splitlines() if ln.startswith("/")]
        self.assertEqual(lines, [small, mid, big])

    def test_the_argv_of_both_children(self) -> None:
        design = self.blif("x/one.blif", design_text())
        code, out, err = self.run_tool("--vtr", "--lut", "4", "--via-blif", "xd", design)
        self.assertEqual(code, 0, out + err)
        work = self.keep / "0000-one"
        (odin3_call,) = self.calls("odin3")
        script = (f"read_techlib lib/vtr.o3lib; read_blif {design}; write_blif "
                  f"{work}/before.blif; abc --lut 4 --via-blif xd; write_blif {work}/after.blif; "
                  f"write_blif --vtr {work}/after.vtr.blif")
        self.assertEqual(odin3_call["argv"], [self.odin3, "-p", script])
        (equiv_call,) = self.calls("equiv")
        self.assertEqual(equiv_call["argv"], [
            self.equiv, "--blackbox", "adder,multiply,single_port_ram,dual_port_ram", "--timeout",
            "600", f"{work}/before.blif", f"{work}/after.blif"])
        self.assertRegex(out, r"one\.blif equivalent [0-9.]+ s\n")

    def test_the_default_odin3_script(self) -> None:
        design = self.blif("one.blif", design_text())
        self.assertEqual(self.run_tool(design)[0], 0)
        (call,) = self.calls("odin3")
        argv = call["argv"]
        assert isinstance(argv, list)
        self.assertIn("abc --lut 6; write_blif", argv[2])
        self.assertNotIn("--vtr", argv[2])

    def test_an_unconn_set_change_is_reported(self) -> None:
        ram = ".subckt spr addr[5]=unconn out[0]=q\n"
        design = self.blif("unconn.blif", design_text(ram) + SPR)
        stable = self.blif("stable.blif", design_text(ram, pad=50) + SPR)
        code, out, err = self.run_tool(design, stable)
        self.assertEqual(code, 1, out + err)
        self.assertRegex(out, r"unconn\.blif unconn-changed [0-9.]+ s \(after\.blif: no longer "
                              r"on unconn \[\('spr', 1, 'addr\[5\]'\)\]")
        self.assertRegex(out, r"stable\.blif equivalent")

    def test_the_expect_list_rules(self) -> None:
        views = self.blif("views.blif", design_text())
        broken = self.blif("broken.blif", design_text(pad=1))
        diff = self.blif("diff.blif", design_text("# differs\n", pad=2))
        fine = self.blif("fine.blif", design_text(pad=3))
        expect = self.blif("expect.txt", "views.blif views\nbroken.blif views\nfine.blif other\n")
        code, out, err = self.run_tool("--expect-fail", expect, views, broken, diff, fine)
        self.assertEqual(code, 1, out + err)
        self.assertRegex(out, r"views\.blif views [0-9.]+ s \(listed: views")
        self.assertRegex(out, r"broken\.blif error [0-9.]+ s \(listed as views but error")
        self.assertRegex(out, r"diff\.blif differs")
        self.assertRegex(out, r"fine\.blif equivalent [0-9.]+ s \(XPASS: listed \(other\)")
        self.assertIn("abc-equiv: 4 designs: 1 passed, 3 failed (differs 1, equivalent 1, "
                      "error 1, views 1)", out)

    def test_the_summary_line(self) -> None:
        a = self.blif("a.blif", design_text())
        b = self.blif("b.blif", design_text(pad=5))
        code, out, _ = self.run_tool(a, b)
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines()[-1], "abc-equiv: 2 designs: 2 passed, 0 failed "
                                               "(equivalent 2)")

    def test_a_listed_multiclock_design_passes(self) -> None:
        multi = self.blif("multi.blif", design_text("# multiclock\n"))
        expect = self.blif("expect.txt", "multi.blif multiclock\n")
        code, out, err = self.run_tool("--expect-fail", expect, multi)
        self.assertEqual(code, 0, out + err)
        self.assertRegex(out, r"multi\.blif multiclock")

    def test_step_abc_then_step_equiv(self) -> None:
        design = self.blif("one.blif", design_text())
        code, out, err = self.run_tool("--step", "abc", design)
        self.assertEqual(code, 0, out + err)
        self.assertRegex(out, r"one\.blif mapped [0-9.]+ s")
        self.assertEqual(self.calls("equiv"), [])
        work = self.keep / "0000-one"
        self.assertTrue((work / "before.blif").is_file() and (work / "after.blif").is_file())
        code, out, err = self.run_tool("--step", "equiv", design)
        self.assertEqual(code, 0, out + err)
        self.assertRegex(out, r"one\.blif equivalent")
        self.assertEqual(len(self.calls("odin3")), 1)
        self.assertEqual(len(self.calls("equiv")), 1)

    def test_step_equiv_with_a_missing_file_is_an_error(self) -> None:
        design = self.blif("one.blif", design_text())
        code, out, _ = self.run_tool("--step", "equiv", design)
        self.assertEqual(code, 1)
        self.assertRegex(out, r"one\.blif error")

    def test_a_step_without_keep_is_a_usage_error(self) -> None:
        design = self.blif("one.blif", design_text())
        for step in ("abc", "equiv"):
            code, out, err = helpers.run_main(abc_equiv.main, [
                "--odin3", self.odin3, "--equiv-check", self.equiv, "--step", step, design])
            self.assertEqual(code, 2, out)
            self.assertIn(f"--step {step} needs --keep DIR", err)

    def test_the_child_environment_is_the_callers(self) -> None:
        design = self.blif("one.blif", design_text())
        self.assertEqual(self.run_tool(design)[0], 0)
        for kind in ("odin3", "equiv"):
            (call,) = self.calls(kind)
            self.assertEqual(call["env"], dict(os.environ))
            env = call["env"]
            assert isinstance(env, dict)
            self.assertNotIn("ODIN3_HEAVY_OK", env)

    def test_exchange_sets_only_that_variable(self) -> None:
        design = self.blif("one.blif", design_text())
        self.assertEqual(self.run_tool("--exchange", "aig", design)[0], 0)
        (call,) = self.calls("odin3")
        self.assertEqual(call["env"], {**os.environ, "ODIN3_ABC_EXCHANGE": "aig"})
        (equiv_call,) = self.calls("equiv")
        equiv_env = equiv_call["env"]
        assert isinstance(equiv_env, dict)
        self.assertNotIn("ODIN3_ABC_EXCHANGE", equiv_env)

    def test_a_golden_path_outside_heavy_run_is_refused(self) -> None:
        golden = self.root / "golden"
        golden.mkdir()
        design = self.blif("golden/g.parmys.blif", design_text())
        refusal = qor_heavy.HeavyError(2, "not inside a heavy run: test")
        with mock.patch.object(abc_equiv, "GOLDEN_ROOT", golden), \
                mock.patch.object(qor_heavy, "require_heavy", side_effect=refusal):
            code, out, err = self.run_tool(design)
        self.assertEqual(code, 2, out)
        self.assertIn("abc-equiv: not inside a heavy run: test", err)
        self.assertEqual(self.calls("odin3"), [])

    def test_a_missing_path_is_a_usage_error(self) -> None:
        code, _, err = self.run_tool(str(self.root / "nowhere"))
        self.assertEqual(code, 2)
        self.assertIn("no such file or directory", err)

    def test_via_blif_and_exchange_exclude_each_other(self) -> None:
        design = self.blif("one.blif", design_text())
        code, _, err = self.run_tool("--via-blif", "d", "--exchange", "aig", design)
        self.assertEqual(code, 2)
        self.assertIn("exclude each other", err)


if __name__ == "__main__":
    unittest.main()

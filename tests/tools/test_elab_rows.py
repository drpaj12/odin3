"""Tests for tests/elab/rows: the coverage rows of 2D (spec §14.2, plan Task 25).

rows.txt lists the MUST and SHOULD rows of the coverage audit's §4 tables (plus N15's uwire); each
has a source with its header, an .elab golden, an .il golden and, with a combinational always,
a .nolatches.il golden. The 54 semantics-heavy rows carry a hand-written .elab. CMakePresets.json
keeps the sweep tests out of the debug and release presets.
"""

from __future__ import annotations

import importlib.machinery
import importlib.util
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "elab-check"))

import run_rows_equiv  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]
ROWS = REPO_ROOT / "tests" / "elab" / "rows"
AUDIT = REPO_ROOT / "docs" / "specs" / "2026-10-09-2C-verilog-coverage.md"
PRESETS = REPO_ROOT / "CMakePresets.json"

ROW_COUNT = 125
HAND_MARK = "# hand-written"
LINE = re.compile(r"^([A-Z]\d+) (\1_[a-z0-9_]+\.v) comb=([01]) clocks=(\d+)$")
_STRIP = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"', re.S)
_ALWAYS = re.compile(r"\balways\s*@\s*(\*|\(\s*\*\s*\)|\([^)]*\)|[A-Za-z_]\w*)")
_EDGE = re.compile(r"\b(?:posedge|negedge)\s+([A-Za-z_]\w*)")


def _span(prefix: str, first: int, last: int) -> set[str]:
    return {f"{prefix}{i}" for i in range(first, last + 1)}


HAND_ROWS = (
    {"M1", "M2", "M12", "P8", "N2", "E8", "E9", "E12", "E15", "E16", "E22", "E23"}
    | {"T7", "T18", "F1", "I3", "I6"}
    | _span("P", 1, 6)
    | _span("N", 7, 13)
    | _span("G", 1, 7)
    | _span("C", 4, 9)
    | _span("K", 1, 11)
)


def audit_rows() -> list[str]:
    """The rows of the audit's §4 tables whose verdict starts with MUST or SHOULD, plus N15."""
    text = AUDIT.read_text(encoding="utf-8")
    section = text.split("\n## 4. ", 1)[1].split("\n## 5. ", 1)[0]
    out = []
    for line in section.splitlines():
        match = re.match(r"\| ([A-Z]\d+) \|", line)
        if not match:
            continue
        verdict = line.strip().strip("|").split("|")[-1].strip()
        if verdict.startswith(("MUST", "SHOULD", "`uwire`")):
            out.append(match.group(1))
    return out


def classify(source: str) -> tuple[bool, int]:
    """(has a combinational always, number of distinct edge signals) of a Verilog source."""
    text = _STRIP.sub(" ", source)
    comb = False
    for match in _ALWAYS.finditer(text):
        if not _EDGE.search(match.group(1)):
            comb = True
    return comb, len({m.group(1) for m in _EDGE.finditer(text)})


def read_rows() -> list[tuple[str, str, bool, int]]:
    out = []
    for line in (ROWS / "rows.txt").read_text(encoding="utf-8").splitlines():
        match = LINE.match(line)
        if match is None:
            raise ValueError(f"rows.txt: bad line {line!r}")
        out.append((match.group(1), match.group(2), match.group(3) == "1", int(match.group(4))))
    return out


class ElabRowsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.rows = read_rows()

    def test_rows_txt_lists_exactly_the_audit_rows(self) -> None:
        listed = [row for row, _, _, _ in self.rows]
        self.assertEqual(len(listed), ROW_COUNT)
        self.assertEqual(sorted(listed), sorted(audit_rows()))
        self.assertEqual(len(set(listed)), ROW_COUNT)

    def test_every_file_exists_with_its_header(self) -> None:
        for row, name, _, _ in self.rows:
            with self.subTest(row=row):
                first = (ROWS / name).read_text(encoding="utf-8").split("\n", 1)[0]
                self.assertRegex(first, rf"^// {row}: .+ — variants: .+")

    def test_comb_and_clocks_match_the_source(self) -> None:
        for row, name, comb, clocks in self.rows:
            with self.subTest(row=row):
                got = classify((ROWS / name).read_text(encoding="utf-8"))
                self.assertEqual(got, (comb, clocks))

    def test_exactly_the_54_hand_rows_are_hand_written(self) -> None:
        self.assertEqual(len(HAND_ROWS), 54)
        for row, _, _, _ in self.rows:
            with self.subTest(row=row):
                first = (ROWS / f"{row}.elab").read_text(encoding="utf-8").split("\n", 1)[0]
                if row in HAND_ROWS:
                    self.assertEqual(first, HAND_MARK)
                else:
                    self.assertTrue(first.startswith("# generated: elab-check --write"), first)

    def test_il_goldens(self) -> None:
        for row, _, comb, _ in self.rows:
            with self.subTest(row=row):
                self.assertTrue((ROWS / f"{row}.il").is_file())
                self.assertEqual((ROWS / f"{row}.nolatches.il").is_file(), comb)

    def test_no_stray_goldens(self) -> None:
        rows = {row for row, _, _, _ in self.rows}
        for path in ROWS.glob("*.elab"):
            self.assertIn(path.stem, rows, path.name)
        for path in ROWS.glob("*.il"):
            self.assertIn(path.name.split(".")[0], rows, path.name)

    def test_expect_fail_lists_known_rows_with_reasons(self) -> None:
        rows = {row for row, _, _, _ in self.rows}
        path = REPO_ROOT / "tests" / "elab" / "equiv_expect_fail.txt"
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                row, _, reason = line.partition(" ")
                self.assertIn(row, rows)
                self.assertTrue(reason.strip(), f"{row}: no reason")


class PresetsTest(unittest.TestCase):
    def setUp(self) -> None:
        data = json.loads(PRESETS.read_text(encoding="utf-8"))
        self.presets = {p["name"]: p for p in data["testPresets"]}

    def test_debug_and_release_exclude_the_sweep_labels(self) -> None:
        for name in ("debug", "release"):
            with self.subTest(preset=name):
                label = self.presets[name]["filter"]["exclude"]["label"]
                for sweep in ("sweep", "sweep-xl"):
                    self.assertIsNotNone(re.search(label, sweep), (name, sweep))
                self.assertIsNone(re.search(label, "elab"))

    def test_sweep_presets(self) -> None:
        sweep = self.presets["sweep"]
        self.assertEqual(sweep["configurePreset"], "release")
        self.assertEqual(sweep["filter"]["include"]["label"], "^sweep$")
        self.assertEqual(sweep["execution"]["jobs"], 1)
        self.assertEqual(sweep["environment"]["ODIN3_HEAVY_OK"], "1")
        xl = self.presets["sweep-xl"]
        self.assertEqual(xl["configurePreset"], "release")
        self.assertEqual(xl["filter"]["include"]["label"], "sweep-xl")
        self.assertNotIn("ODIN3_HEAVY_XL_OK", json.dumps(self.presets))


ELAB_CHECK = os.environ.get("ODIN3_ELAB_CHECK")
SOURCE = "module t (input a, output y);\n  assign y = ~a;\nendmodule\n"


def run_check(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    assert ELAB_CHECK
    return subprocess.run(
        [ELAB_CHECK, *args], capture_output=True, text=True, check=False, timeout=120, cwd=cwd
    )


@unittest.skipIf(not ELAB_CHECK, "ODIN3_ELAB_CHECK not set (run through CTest)")
class ElabCheckTest(unittest.TestCase):
    """elab-check's exit codes and golden handling, on a one-line module in a scratch directory."""

    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp(prefix="elab-check-test-"))
        self.src = self.dir / "Z1_not.v"
        self.src.write_text(SOURCE, encoding="utf-8")

    def tearDown(self) -> None:
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_write_then_check_passes_in_both_modes(self) -> None:
        self.assertEqual(run_check("--write", str(self.src)).returncode, 0)
        self.assertEqual(run_check("--write", "--nolatches", str(self.src)).returncode, 0)
        self.assertEqual(run_check(str(self.src)).returncode, 0)
        self.assertEqual(run_check("--nolatches", str(self.src)).returncode, 0)
        self.assertEqual(run_check("--stage1", str(self.src)).returncode, 0)
        first = (self.dir / "Z1.elab").read_text(encoding="utf-8").split("\n", 1)[0]
        self.assertEqual(first, "# generated: elab-check --write")

    def test_a_relative_path_with_a_directory_finds_the_goldens(self) -> None:
        rel = f"{self.dir.name}/Z1_not.v"
        parent = self.dir.parent
        self.assertEqual(run_check("--write", rel, cwd=parent).returncode, 0)
        proc = run_check(rel, cwd=parent)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(run_check("--stage1", rel, cwd=parent).returncode, 0)
        self.assertTrue((self.dir / "Z1.elab").exists())

    def test_the_in_file_auto_blackbox_directive(self) -> None:
        text = "module t (inout a, inout y);\n  nosuch u (.a(a), .y(y));\nendmodule\n"
        plain = self.dir / "Z3_bb.v"
        plain.write_text(text, encoding="utf-8")
        self.assertEqual(run_check("--write", str(plain)).returncode, 1)
        marked = self.dir / "Z4_bb.v"
        marked.write_text("// Z4: row header\n// elab-check: --auto-blackbox\n" + text, encoding="utf-8")
        self.assertEqual(run_check("--write", str(marked)).returncode, 0)
        self.assertEqual(run_check(str(marked)).returncode, 0)

    def test_a_changed_golden_fails_with_the_line(self) -> None:
        run_check("--write", str(self.src))
        il = self.dir / "Z1.il"
        il.write_text(il.read_text(encoding="utf-8").replace("assign", "assignX", 1) + "x\n")
        proc = run_check(str(self.src))
        self.assertEqual(proc.returncode, 1, proc.stderr)
        self.assertIn("differs from the golden at line", proc.stderr)
        self.assertEqual(run_check("--stage1", str(self.src)).returncode, 0)

    def test_a_missing_golden_fails(self) -> None:
        proc = run_check(str(self.src))
        self.assertEqual(proc.returncode, 1)
        self.assertIn("no golden", proc.stderr)

    def test_write_refuses_a_hand_written_golden(self) -> None:
        run_check("--write", str(self.src))
        elab = self.dir / "Z1.elab"
        body = elab.read_text(encoding="utf-8").split("\n", 1)[1]
        elab.write_text("# hand-written\n# PROVISIONAL: note\n" + body, encoding="utf-8")
        proc = run_check("--write", str(self.src))
        self.assertEqual(proc.returncode, 2)
        self.assertIn("refusing to overwrite a hand-written golden", proc.stderr)
        self.assertEqual(run_check(str(self.src)).returncode, 0)  # the note lines are not body

    def test_an_elaboration_error_fails_and_usage_errors_exit_2(self) -> None:
        bad = self.dir / "Z2_bad.v"
        bad.write_text("module t (output y);\n  assign y = nosuch;\nendmodule\n")
        self.assertEqual(run_check(str(bad)).returncode, 1)
        self.assertEqual(run_check().returncode, 2)
        self.assertEqual(run_check("--bogus", str(self.src)).returncode, 2)
        self.assertEqual(run_check(str(self.dir / "missing.v")).returncode, 2)


FAKE_BRIDGE = """#!{python}
import sys
name = sys.argv[-1].rsplit("/", 1)[-1]
sys.exit({{"A1_a.v": 0, "B1_b.v": 1, "C1_c.v": 0}}.get(name, 0))
"""


class RunRowsEquivTest(unittest.TestCase):
    """The sweep driver over a fake bridge: unexpected results, stale entries, clocks, order."""

    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp(prefix="rows-equiv-test-"))
        for name, size in (("A1_a.v", 30), ("B1_b.v", 20), ("C1_c.v", 10)):
            (self.dir / name).write_text("//" + "x" * size + "\n", encoding="utf-8")
        self.rows = [
            run_rows_equiv.Row("A1", self.dir / "A1_a.v", 1),
            run_rows_equiv.Row("B1", self.dir / "B1_b.v", 1),
            run_rows_equiv.Row("C1", self.dir / "C1_c.v", 3),
        ]
        self.bridge = self.dir / "bridge"
        self.bridge.write_text(FAKE_BRIDGE.format(python=sys.executable), encoding="utf-8")
        self.bridge.chmod(self.bridge.stat().st_mode | stat.S_IXUSR)

    def tearDown(self) -> None:
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_rows_are_listed_smallest_first(self) -> None:
        (self.dir / "rows.txt").write_text(
            "A1 A1_a.v comb=0 clocks=1\nB1 B1_b.v comb=0 clocks=1\nC1 C1_c.v comb=0 clocks=3\n"
        )
        order = [r.row for r in run_rows_equiv.read_rows(self.dir)]
        self.assertEqual(order, ["C1", "B1", "A1"])

    def test_unexpected_failure_and_stale_entry(self) -> None:
        bridge = [str(self.bridge)]
        self.assertEqual(run_rows_equiv.sweep(bridge, self.rows, {}), 1)  # B1 fails
        self.assertEqual(run_rows_equiv.sweep(bridge, self.rows, {"B1": "why"}), 0)
        self.assertEqual(run_rows_equiv.sweep(bridge, self.rows, {"A1": "x", "B1": "y"}), 1)

    def test_a_row_with_several_clocks_is_not_run(self) -> None:
        bridge = [str(self.bridge)]
        rows = [r for r in self.rows if r.row == "C1"]
        self.assertEqual(run_rows_equiv.sweep(bridge, rows, {}), 0)

    def test_expect_fail_needs_a_reason(self) -> None:
        path = self.dir / "expect.txt"
        path.write_text("# c\nB1 because\n\n", encoding="utf-8")
        self.assertEqual(run_rows_equiv.read_expect_fail(path), {"B1": "because"})
        path.write_text("B1\n", encoding="utf-8")
        with self.assertRaises(ValueError):
            run_rows_equiv.read_expect_fail(path)


def load_micro_top() -> Any:
    path = str(REPO_ROOT / "tools" / "elab-check" / "micro-top")
    loader = importlib.machinery.SourceFileLoader("micro_top", path)
    spec = importlib.util.spec_from_loader("micro_top", loader)
    assert spec is not None
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class MicroTopTest(unittest.TestCase):
    """micro-top reads the first .model of a micro's Parmys golden (plan point (o))."""

    def test_first_model_of_the_golden(self) -> None:
        mod = load_micro_top()
        with tempfile.TemporaryDirectory(prefix="micro-top-test-") as tmp:
            leaf = Path(tmp) / "regression" / "verilog" / "grp" / "leaf1"
            leaf.mkdir(parents=True)
            (leaf / "leaf1.parmys.blif").write_text(
                "# Generated\n\n.model topmod\n.inputs a\n.end\n.model second\n.end\n"
            )
            self.assertEqual(
                mod.group_and_leaf("tests/micro/verilog/grp/leaf1.v"), ("grp", "leaf1")
            )
            self.assertEqual(mod.group_and_leaf("grp/leaf1"), ("grp", "leaf1"))
            self.assertEqual(mod.main(["--golden-root", tmp, "grp/leaf1"]), 0)
            self.assertEqual(mod.main(["--golden-root", tmp, "grp/none"]), 1)
            (leaf / "leaf1.parmys.blif").write_text("# no model\n")
            self.assertEqual(mod.main(["--golden-root", tmp, "grp/leaf1"]), 1)


if __name__ == "__main__":
    unittest.main()

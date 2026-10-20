"""The committed hardmap lists (3C Task 7): the 19, the micros, the expect files and the divergence
rows agree with the spec and with each other."""

from __future__ import annotations

import re
import unittest
from pathlib import Path

import hardmap_diff as hd

from .helpers import REPO_ROOT

HM = REPO_ROOT / "tests" / "hardmap"
SPEC = REPO_ROOT / "docs" / "specs" / "2026-10-14-3C-hardmap-design.md"
SURVEY_ROW = re.compile(r"^\| `(\w+)` \|", re.MULTILINE)
D_ROW = re.compile(r"^\| (D\d+[a-z]?) \|", re.MULTILINE)


def names(path: Path) -> list[str]:
    return [
        ln.strip() for ln in path.read_text().splitlines() if ln.strip() and not ln.startswith("#")
    ]


def survey_designs() -> set[str]:
    text = SPEC.read_text()
    section = text[text.index("## 2. Survey") : text.index("**Findings:**")]
    return set(SURVEY_ROW.findall(section))


class ListTests(unittest.TestCase):
    def test_the_19_are_the_spec_survey(self) -> None:
        designs = names(HM / "vtr_designs.txt")
        self.assertEqual(len(designs), 19)
        self.assertEqual(len(set(designs)), 19)
        self.assertEqual(set(designs), survey_designs())

    def test_mcml_and_lu32_run_last(self) -> None:
        self.assertEqual(names(HM / "vtr_designs.txt")[-2:], ["mcml", "LU32PEEng"])

    def test_micros_exist_and_are_unique(self) -> None:
        micros = names(HM / "micros.txt")
        self.assertEqual(len(micros), len(set(micros)))
        for micro in micros:
            self.assertTrue((REPO_ROOT / "tests" / "micro" / "verilog" / micro).is_file(), micro)

    def test_expect_files_parse_and_name_known_designs(self) -> None:
        micros = set(names(HM / "micros.txt"))
        vtr = {f"{name}.v" for name in names(HM / "vtr_designs.txt")}
        for file, known in (("diff_expect.txt", micros), ("vtr_expect.txt", vtr)):
            table = hd.read_expect([HM / file])
            for path in table:
                self.assertIn(path, known, f"{file}: {path}")

    def test_divergence_ids_are_in_the_spec_table(self) -> None:
        spec_ids = set(D_ROW.findall(SPEC.read_text()))
        files = [HM / "diff_expect.txt", HM / "vtr_expect.txt"]
        files += sorted((REPO_ROOT / "tests" / "partial_map").glob("diff_expect_*.txt"))
        for file in files:
            for ident in re.findall(r"\b(D\d+[a-z]?)\b", file.read_text()):
                self.assertIn(ident, spec_ids, f"{file.name}: {ident}")

    def test_dual_write_race_rows(self) -> None:
        rows = [
            ln.split("\t")
            for ln in (REPO_ROOT / "tests" / "vtr" / "qor" / "divergences.tsv")
            .read_text()
            .splitlines()
            if ln and not ln.startswith("#")
        ]
        races = {row[0] for row in rows if row[1] == "dual-write-race"}
        self.assertEqual(races, {"LU8PEEng", "LU32PEEng"})


if __name__ == "__main__":
    unittest.main()

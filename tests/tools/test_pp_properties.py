"""The preprocessor's files-read property (spec 2B §10.5): for every Phase 2 positive case of
the project corpus (tests/golden/projects), in every format whose reader has landed,
`read_project; preprocess --out DIR` reads (DIR/files-read.txt) a subset of the case's
`resolved.read` without its non-Verilog design files, and exactly that set when the format's
record has no `libfiles`/`libdirs` (library files are loaded on demand by 2D, `lib_v_y`).

CTest `pp_properties` runs this with ODIN3_BIN set to the odin3 executable; without it (as under
`python_tools`) the test is skipped. The other §10.5 properties are C unit tests
(tests/unit/test_pp_props.c)."""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from typing import Any

import project_fixtures as pf

ODIN3 = os.path.abspath(os.environ["ODIN3_BIN"]) if os.environ.get("ODIN3_BIN") else ""


def phase2_positive_cases() -> list[tuple[Path, dict[str, Any]]]:
    """Every Phase 2 positive case, smallest first (memory rule)."""
    out = []
    for d in sorted(pf.case_dirs(pf.DEFAULT_ROOT, []), key=lambda d: (pf.source_bytes(d), d.name)):
        exp = json.loads((d / "expected.json").read_text(encoding="utf-8"))
        if exp["phase"] == 2 and "error" not in exp:
            out.append((d, exp))
    return out


def wanted(exp: dict[str, Any], fmt: str) -> tuple[set[str], bool]:
    """The files the preprocessor may read in fmt, and whether it must read all of them."""
    record = {**exp["project"], **exp.get("format_overrides", {}).get(fmt, {})}
    other = {f["path"] for f in record["files"] if f["language"] != "verilog"}
    exact = not record["libfiles"] and not record["libdirs"]
    return set(exp["resolved"]["read"]) - other, exact


@unittest.skipUnless(ODIN3, "ODIN3_BIN is not set (CTest pp_properties sets it)")
class FilesReadTest(unittest.TestCase):
    def test_files_read_within_resolved_read(self) -> None:
        checked = 0
        for case_dir, exp in phase2_positive_cases():
            for fmt in exp["formats"]:
                gate = exp.get("format_phase", {}).get(fmt)
                if gate is not None and gate not in pf.LANDED_FORMAT_PHASES:
                    continue
                checked += 1
                with self.subTest(case=case_dir.name, format=fmt):
                    self.check(case_dir, exp, fmt)
        self.assertGreaterEqual(checked, 40)

    def check(self, case_dir: Path, exp: dict[str, Any], fmt: str) -> None:
        case = os.path.abspath(case_dir)
        entry = Path(os.path.abspath(pf.entry_of(case_dir, fmt, exp)))
        with tempfile.TemporaryDirectory() as out:
            script = pf.compare_script(fmt, exp, entry, f"preprocess --out {pf.script_word(out)}")
            proc = pf.run_odin3(ODIN3, script, exp.get("env", {}))
            self.assertEqual(0, proc.returncode, proc.stderr)
            lines = Path(out, "files-read.txt").read_text(encoding="utf-8").splitlines()
        read = [pf.rel_to(case, p) for p in lines]
        self.assertEqual(len(read), len(set(read)), f"a file read twice: {read}")
        want, exact = wanted(exp, fmt)
        if exact:
            self.assertEqual(want, set(read))
        else:
            self.assertLessEqual(set(read), want)


if __name__ == "__main__":
    unittest.main()

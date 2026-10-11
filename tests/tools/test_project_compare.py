"""Tests for `project-fixtures compare` (spec 2B §10.2): the corpus harness that runs `odin3`
on every case and format and compares its dumped record or located error with expected.json.
These run over a fake `odin3` script that answers with canned outputs and logs how it was run;
CTest `project_corpus` runs the same harness over the real binary."""

from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import project_fixtures as pf

from . import helpers

FAKE = """#!{python}
import json, os, sys
CASE = {case!r}
RESPONSES = json.loads({responses!r})
with open({log!r}, "a", encoding="utf-8") as fh:
    fh.write(json.dumps({{"argv": sys.argv[1:], "cwd": os.getcwd(),
                          "env": dict(os.environ)}}) + "\\n")
script = sys.argv[-1]
for resp in RESPONSES:
    if resp["match"] in script:
        sys.stdout.write(resp["stdout"].replace("{{CASE}}", CASE))
        sys.stderr.write(resp["stderr"].replace("{{CASE}}", CASE))
        sys.exit(resp["exit"])
sys.stderr.write("fake odin3: no response for " + script + "\\n")
sys.exit(99)
"""

RECORD: dict[str, Any] = {
    **pf.empty_record(),
    "files": [{"path": "src/top.v", "language": "verilog", "library": "work"}],
    "incdirs": ["inc"],
    "top": "top",
}


def absolute(record: dict[str, Any]) -> dict[str, Any]:
    """The record as odin3 dumps it: every path absolute ({CASE} is the case directory)."""
    out: dict[str, Any] = json.loads(json.dumps(record))
    for item in out["files"] + out["libfiles"]:
        item["path"] = "{CASE}/" + item["path"]
    for key in ("libdirs", "incdirs", "duplicates"):
        out[key] = ["{CASE}/" + p for p in out[key]]
    return out


def dump(record: dict[str, Any]) -> str:
    return json.dumps(absolute(record), indent=2) + "\n"


class CompareTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name) / "corpus"
        self.log = Path(self._tmp.name) / "runs.jsonl"
        self.fake = Path(self._tmp.name) / "odin3"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def case(self, name: str, exp: dict[str, Any], size: int = 10) -> Path:
        case_dir = self.root / name
        (case_dir / "src").mkdir(parents=True)
        (case_dir / "src" / "top.v").write_text("x" * size)
        base = {"description": name, "features": ["verilog"], "phase": 2}
        (case_dir / "expected.json").write_text(json.dumps({**base, **exp}))
        return case_dir

    def respond(self, case_dir: Path, responses: list[dict[str, Any]]) -> None:
        full = [{"stdout": "", "stderr": "", "exit": 0, **r} for r in responses]
        self.fake.write_text(FAKE.format(python=sys.executable, case=str(case_dir),
                                         responses=json.dumps(full), log=str(self.log)))
        self.fake.chmod(0o755)

    def compare(self, *extra: str) -> tuple[int, str]:
        code, out, err = helpers.run_main(pf.main, ["compare", "--odin3", str(self.fake),
                                                    "--root", str(self.root), *extra])
        return code, out + err

    def runs(self) -> list[dict[str, Any]]:
        if not self.log.is_file():
            return []
        return [json.loads(line) for line in self.log.read_text().splitlines()]

    # ---- positive cases --------------------------------------------------------------------

    def test_match(self) -> None:
        d = self.case("pos", {"formats": ["o3proj"], "project": RECORD, "env": {"V": "1"}})
        self.respond(d, [{"match": "--format o3proj", "stdout": dump(RECORD)}])
        code, out = self.compare()
        self.assertEqual(code, 0, out)
        self.assertIn("ok   pos o3proj", out)
        [run] = self.runs()
        entry = os.path.abspath(d / "project.o3proj")
        self.assertEqual(run["argv"], ["--diag-paths", "resolved", "-p",
                                       f"read_project --format o3proj {entry}; dump_project"])
        self.assertNotEqual(Path(run["cwd"]).resolve(), d.resolve())
        self.assertEqual(set(run["env"]) - {"LC_CTYPE"}, {"PATH", "V"})
        self.assertEqual(run["env"]["V"], "1")

    def test_record_mismatch(self) -> None:
        d = self.case("pos", {"formats": ["o3proj"], "project": RECORD})
        self.respond(d, [{"match": "o3proj", "stdout": dump({**RECORD, "top": "other"})}])
        code, out = self.compare()
        self.assertEqual(code, 1)
        self.assertIn("FAIL pos o3proj", out)
        self.assertIn('record top: expected "top", got "other"', out)

    def test_positive_needs_exit_zero_and_json(self) -> None:
        d = self.case("pos", {"formats": ["o3proj", "f"], "project": RECORD})
        self.respond(d, [{"match": "o3proj", "stdout": dump(RECORD), "exit": 1},
                         {"match": "--format f", "stdout": "not json"}])
        code, out = self.compare()
        self.assertEqual(code, 1)
        self.assertIn("exit 1", out)
        self.assertIn("f: stdout is not JSON", out)

    def test_key_order(self) -> None:
        d = self.case("pos", {"formats": ["o3proj"], "project": RECORD})
        rec = absolute(RECORD)
        swapped = {"libfiles": rec["libfiles"], "files": rec["files"],
                   **{k: v for k, v in rec.items() if k not in ("files", "libfiles")}}
        self.respond(d, [{"match": "o3proj", "stdout": json.dumps(swapped)}])
        code, out = self.compare()
        self.assertEqual(code, 1)
        self.assertIn("keys", out)

    def test_relative_path_in_record_is_not_hidden(self) -> None:
        d = self.case("pos", {"formats": ["o3proj"], "project": RECORD})
        rec = absolute(RECORD)
        rec["incdirs"] = ["inc"]  # odin3 must dump resolved (absolute) paths
        self.respond(d, [{"match": "o3proj", "stdout": json.dumps(rec)}])
        code, out = self.compare()
        self.assertEqual(code, 1)
        self.assertIn("not absolute", out)

    def test_every_path_field_relativized(self) -> None:
        rec = {**RECORD, "libfiles": [{"path": "lib/cells.v", "language": "verilog"}],
               "libdirs": ["../shared"],
               "arch": [{"kind": "o3lib", "path": "arch/a.o3lib"},
                        {"kind": "device", "family": "/not/a/path", "device": "D"}],
               "rules": [{"rule": "patterns", "path": "arch/p.o3lib"},
                         {"rule": "limit", "cell": "multiply", "count": 4}],
               "defines": [{"name": "P", "value": "/abs/value"}],
               "flow": "flow/s.o3", "output": {"path": "out/x.blif", "format": "blif"}}
        d = self.case("pos", {"formats": ["o3proj"], "project": rec})
        dumped = absolute(rec)
        dumped["arch"][0]["path"] = "{CASE}/arch/a.o3lib"
        dumped["rules"][0]["path"] = "{CASE}/arch/p.o3lib"
        dumped["flow"] = "{CASE}/flow/s.o3"
        dumped["output"]["path"] = "{CASE}/out/x.blif"
        dumped["libdirs"] = [str(self.root / "shared")]
        self.respond(d, [{"match": "o3proj", "stdout": json.dumps(dumped)}])
        code, out = self.compare()
        self.assertEqual(code, 0, out)

    def test_output_without_path_is_reported(self) -> None:
        d = self.case("pos", {"formats": ["o3proj"], "project": RECORD})
        dumped = absolute(RECORD)
        dumped["output"] = {"format": "blif"}
        self.respond(d, [{"match": "o3proj", "stdout": json.dumps(dumped)}])
        code, out = self.compare()
        self.assertEqual(code, 1)
        self.assertIn("malformed record: missing key 'path'", out)
        self.assertNotIn("Traceback", out)

    def test_duplicates_resorted_after_relativizing(self) -> None:
        # Absolute: <root>/pos/src/a.v sorts before <root>/zz/d.v; relative: ../zz/d.v first.
        rec = {**RECORD, "duplicates": ["../zz/d.v", "src/a.v"]}
        d = self.case("pos", {"formats": ["f"], "project": rec})
        dumped = absolute(rec)
        dumped["duplicates"] = ["{CASE}/src/a.v", str(self.root / "zz" / "d.v")]
        entry = os.path.abspath(d / "files.f")
        info = (f"odin3: info: {entry}: listed more than once, read once: "
                f"{d}/src/a.v, {self.root}/zz/d.v\n")
        self.respond(d, [{"match": "files.f", "stdout": json.dumps(dumped), "stderr": info}])
        code, out = self.compare()
        self.assertEqual(code, 0, out)

    def test_info_lines_required_when_non_empty(self) -> None:
        rec = {**RECORD, "ignored": ["LOCATION", "SDC_FILE"]}
        d = self.case("pos", {"formats": ["o3proj", "f"], "project": rec})
        o3 = os.path.abspath(d / "project.o3proj")
        good = f"odin3: info: {o3}: ignored: LOCATION, SDC_FILE\n"
        self.respond(d, [{"match": "o3proj", "stdout": dump(rec), "stderr": good},
                         {"match": "files.f", "stdout": dump(rec),
                          "stderr": "odin3: info: files.f: ignored: LOCATION\n"}])
        code, out = self.compare()
        self.assertEqual(code, 1)
        self.assertIn("ok   pos o3proj", out)
        self.assertIn("FAIL pos f", out)
        self.assertIn("info line", out)

    def test_info_line_absent_when_empty(self) -> None:
        d = self.case("pos", {"formats": ["o3proj"], "project": RECORD})
        o3 = os.path.abspath(d / "project.o3proj")
        self.respond(d, [{"match": "o3proj", "stdout": dump(RECORD),
                          "stderr": f"odin3: info: {o3}: ignored: X\n"}])
        code, out = self.compare()
        self.assertEqual(code, 1)
        self.assertIn("unexpected info line", out)

    def test_override_applied(self) -> None:
        exp = {"formats": ["o3proj", "qsf"], "project": RECORD,
               "format_overrides": {"qsf": {"top": "work.top", "libdirs": ["inc"]}},
               "options": {"qsf": {"revision": "fast"}}}
        d = self.case("pos", exp)
        (d / "proj.qpf").write_text("")
        over = {**RECORD, "top": "work.top", "libdirs": ["inc"]}
        self.respond(d, [{"match": "o3proj", "stdout": dump(RECORD)},
                         {"match": "--format qsf", "stdout": dump(over)}])
        code, out = self.compare()
        self.assertEqual(code, 0, out)
        qsf = [r for r in self.runs() if "--format qsf" in r["argv"][-1]]
        qpf = os.path.abspath(d / "proj.qpf")
        self.assertEqual(qsf[0]["argv"][-1],
                         f"read_project --format qsf --revision fast {qpf}; dump_project")

    def test_format_skipped_until_its_phase_lands(self) -> None:
        d = self.case("pos", {"formats": ["o3proj", "odin2"], "project": RECORD,
                              "format_phase": {"odin2": "odin2-reader"}})
        self.respond(d, [{"match": "o3proj", "stdout": dump(RECORD)}])
        with mock.patch.object(pf, "LANDED_FORMAT_PHASES", frozenset()):
            code, out = self.compare()
        self.assertEqual(code, 0, out)
        self.assertIn("skip pos odin2 (format_phase odin2-reader)", out)
        self.assertEqual(len(self.runs()), 1)
        self.assertIn("odin2 0 ok, 0 failed, 1 skipped", out)

    def test_odin2_runs_once_its_reader_lands(self) -> None:
        over = {**RECORD, "output": {"path": "out.blif", "format": "blif"}}
        d = self.case("pos", {"formats": ["o3proj", "odin2"], "project": RECORD,
                              "format_phase": {"odin2": "odin2-reader"},
                              "format_overrides": {"odin2": {"output": over["output"]}}})
        got = absolute(over)
        got["output"]["path"] = "{CASE}/out.blif"
        self.respond(d, [{"match": "--format o3proj", "stdout": dump(RECORD)},
                         {"match": "--format odin2", "stdout": json.dumps(got, indent=2) + "\n"}])
        code, out = self.compare()
        self.assertEqual(code, 0, out)
        self.assertIn("ok   pos odin2", out)
        self.assertIn("odin2 1 ok, 0 failed, 0 skipped", out)
        xml = os.path.abspath(d / "odin2.xml")
        self.assertEqual(self.runs()[-1]["argv"][-1],
                         f"read_project --format odin2 {xml}; dump_project")

    def test_format_filter_and_case_filter(self) -> None:
        d = self.case("pos", {"formats": ["o3proj", "f"], "project": RECORD})
        self.case("other", {"formats": ["f"], "project": RECORD})
        self.respond(d, [{"match": "o3proj", "stdout": dump(RECORD)}])
        code, out = self.compare("--case", "pos", "--format", "o3proj")
        self.assertEqual(code, 0, out)
        self.assertEqual(len(self.runs()), 1)

    def test_smallest_case_first(self) -> None:
        big = self.case("a_big", {"formats": ["o3proj"], "project": RECORD}, size=5000)
        self.case("b_small", {"formats": ["o3proj"], "project": RECORD}, size=1)
        self.respond(big, [{"match": "o3proj", "stdout": "{}"}])
        self.compare()
        order = [Path(r["argv"][-1].split()[3]).parent.name for r in self.runs()]
        self.assertEqual(order, ["b_small", "a_big"])

    # ---- negative cases --------------------------------------------------------------------

    def neg(self, error: dict[str, Any], stderr: str, exit_code: int = 1,
            formats: tuple[str, ...] = ("o3proj",), **extra: Any) -> tuple[int, str]:
        d = self.case("neg", {"formats": list(formats), "error": error, **extra})
        self.respond(d, [{"match": "read_project", "stderr": stderr, "exit": exit_code}])
        return self.compare()

    MISSING = {"kind": "missing_file", "at": "project.o3proj:3", "names": ["src/missing.v"]}
    MISSING_ERR = ("odin3: error: {CASE}/project.o3proj:3:14: error: missing_file: file not "
                   "found: '{CASE}/src/missing.v'\n  included from {CASE}/x.f:1\n"
                   "odin3: error: pass read_project: failed: ODIN3_ERR_PARSE\n")

    def test_negative_match_relativizes_detail(self) -> None:
        code, out = self.neg(self.MISSING, self.MISSING_ERR)
        self.assertEqual(code, 0, out)
        self.assertIn("ok   neg o3proj", out)
        self.assertIn("; dump_project", self.runs()[0]["argv"][-1])

    def test_negative_needs_exit_one(self) -> None:
        for status in (0, 2, 23):  # success, another failure, an ASan report after the diagnostic
            with self.subTest(status=status):
                self.tearDown()
                self.setUp()
                code, out = self.neg(self.MISSING, self.MISSING_ERR, exit_code=status)
                self.assertEqual(code, 1)
                self.assertIn(f"exit: expected 1, got {status}", out)

    def test_exactly_one_error_line(self) -> None:
        line = self.MISSING_ERR.splitlines()[0] + "\n"
        code, out = self.neg(self.MISSING, line + line)
        self.assertEqual(code, 1)
        self.assertIn("expected one error line, got 2", out)

    def test_wrong_kind(self) -> None:
        code, out = self.neg(self.MISSING, self.MISSING_ERR.replace("missing_file", "syntax"))
        self.assertEqual(code, 1)
        self.assertIn("kind: expected 'missing_file', got 'syntax'", out)

    def test_wrong_line(self) -> None:
        code, out = self.neg(self.MISSING, self.MISSING_ERR.replace(":3:14:", ":4:14:"))
        self.assertEqual(code, 1)
        self.assertIn("at: expected 'project.o3proj:3', got 'project.o3proj:4'", out)

    def test_missing_name(self) -> None:
        code, out = self.neg(self.MISSING, self.MISSING_ERR.replace("src/missing.v", "src/m.v"))
        self.assertEqual(code, 1)
        self.assertIn("name 'src/missing.v' not in detail", out)

    def test_no_error_line(self) -> None:
        code, out = self.neg(self.MISSING, "odin3: error: pass read_project: failed\n")
        self.assertEqual(code, 1)
        self.assertIn("no '<kind>: <detail>' error", out)

    def test_at_per_format_file_level_and_unlocated(self) -> None:
        err = {"kind": "unknown_revision", "at": {"o3proj": "project.o3proj:2", "qsf": None,
                                                  "f": "top.qsf"}, "names": ["nope"]}
        d = self.case("neg", {"formats": ["o3proj", "f", "qsf"], "error": err,
                              "entry": {"qsf": "top.qsf"},
                              "options": {"qsf": {"revision": "nope"}}})
        self.respond(d, [
            {"match": "o3proj", "exit": 1, "stderr": "odin3: error: {CASE}/project.o3proj:2:1: "
             "error: unknown_revision: no such revision: 'nope'\n"},
            {"match": "--format qsf", "exit": 1,
             "stderr": "odin3: error: unknown_revision: no such revision: 'nope'\n"},
            {"match": "--format f", "exit": 1,
             "stderr": "odin3: error: {CASE}/top.qsf: error: unknown_revision: 'nope'\n"}])
        code, out = self.compare()
        self.assertEqual(code, 0, out)
        qsf = [r for r in self.runs() if "--format qsf" in r["argv"][-1]][0]
        self.assertIn("--revision nope", qsf["argv"][-1])

    def test_doubled_prefix_rejected(self) -> None:
        err = {"kind": "unknown_revision", "at": None, "names": ["nope"]}
        code, out = self.neg(err, "odin3: error: odin3: error: unknown_revision: no such "
                             "revision: 'nope'\n")
        self.assertEqual(code, 1)
        self.assertIn("doubled 'odin3: error: ' prefix", out)

    def test_unprefixed_line_is_not_a_diagnostic(self) -> None:
        code, out = self.neg(self.MISSING, "{CASE}/project.o3proj:3:14: error: missing_file: "
                             "'{CASE}/src/missing.v'\n")
        self.assertEqual(code, 1)
        self.assertIn("no '<kind>: <detail>' error", out)

    def test_unlocated_where_a_line_is_expected(self) -> None:
        code, out = self.neg(self.MISSING, "odin3: error: missing_file: '{CASE}/src/missing.v'\n")
        self.assertEqual(code, 1)
        self.assertIn("at: expected 'project.o3proj:3', got None", out)

    def test_include_kinds_run_preprocess(self) -> None:
        err = {"kind": "include_cycle", "at": "src/b.vh:2", "names": ["a.vh"]}
        code, out = self.neg(err, "odin3: error: {CASE}/src/b.vh:2:1: error: include_cycle: "
                             "'a.vh'\n  included from {CASE}/src/a.vh:2\n")
        self.assertEqual(code, 0, out)
        self.assertTrue(self.runs()[0]["argv"][-1].endswith("; preprocess"))

    def test_frontend_case_skipped_without_flag(self) -> None:
        err = {"kind": "unresolved_module", "at": "src/top.v:6", "names": ["missing_cell"]}
        code, out = self.neg(err, "odin3: error: {CASE}/src/top.v:6:5: error: "
                             "unresolved_module: 'missing_cell'\n")
        self.assertIn("skip neg o3proj (needs the front end: --with-frontend)", out)
        self.assertEqual(self.runs(), [])
        self.assertEqual(code, 1)  # nothing was run
        self.assertIn("no case was run", out)
        code, out = self.compare("--with-frontend")
        self.assertEqual(code, 0, out)
        self.assertTrue(self.runs()[0]["argv"][-1].endswith("; read_verilog"))

    def test_usage_errors(self) -> None:
        self.respond(self.root, [])
        self.root.mkdir()
        code, out = self.compare()
        self.assertEqual(code, 2)
        self.assertIn("no cases", out)
        self.case("pos", {"formats": ["o3proj"], "project": RECORD})
        self.fake.unlink()
        code, out = self.compare()
        self.assertEqual(code, 2)
        self.assertIn("no such odin3 executable", out)

    def test_nothing_run_is_a_failure(self) -> None:
        d = self.case("pos", {"formats": ["o3proj"], "project": RECORD})
        self.respond(d, [])
        code, out = self.compare("--format", "f")
        self.assertEqual(code, 1)
        self.assertIn("no case was run", out)

    # ---- strictness --------------------------------------------------------------------------

    UNNORMALIZED = ("{CASE}/src/./top.v", "{CASE}//src/top.v", "{CASE}/src/../src/top.v",
                    "{CASE}/inc/")

    def test_unnormalized_record_path(self) -> None:
        d = self.case("pos", {"formats": ["o3proj"], "project": RECORD})
        for bad in self.UNNORMALIZED:
            with self.subTest(bad=bad):
                rec = absolute(RECORD)
                rec["files" if "top.v" in bad else "incdirs"] = (
                    [{**rec["files"][0], "path": bad}] if "top.v" in bad else [bad])
                self.respond(d, [{"match": "o3proj", "stdout": json.dumps(rec)}])
                code, out = self.compare()
                self.assertEqual(code, 1)
                self.assertIn("is not normalized", out)

    def test_unnormalized_error_paths(self) -> None:
        d = self.case("neg", {"formats": ["o3proj"], "error": self.MISSING})
        for bad in ("{CASE}/./", "{CASE}//", "{CASE}/src/../"):
            for place, err in (
                    ("at", self.MISSING_ERR.replace("{CASE}/project", bad + "project")),
                    ("name", self.MISSING_ERR.replace("{CASE}/src/missing", bad + "src/missing"))):
                with self.subTest(bad=bad, place=place):
                    self.respond(d, [{"match": "read_project", "stderr": err, "exit": 1}])
                    code, out = self.compare()
                    self.assertEqual(code, 1, err)
                    self.assertIn("is not normalized", out)

    def test_unnormalized_info_line_paths(self) -> None:
        rec = {**RECORD, "duplicates": ["src/top.v"]}
        d = self.case("pos", {"formats": ["f"], "project": rec})
        entry = os.path.abspath(d / "files.f")
        for bad in ("{CASE}/./", "{CASE}//", "{CASE}/src/../"):
            for place, line in (
                    ("file", f"{bad}files.f: listed more than once, read once: {{CASE}}/src/top.v"),
                    ("item", f"{entry}: listed more than once, read once: {bad}src/top.v")):
                with self.subTest(bad=bad, place=place):
                    self.respond(d, [{"match": "files.f", "stdout": dump(rec),
                                      "stderr": f"odin3: info: {line}\n"}])
                    code, out = self.compare()
                    self.assertEqual(code, 1, line)
                    self.assertIn("is not normalized", out)

    def test_info_line_entry_must_be_absolute(self) -> None:
        rec = {**RECORD, "ignored": ["LOCATION"]}
        d = self.case("pos", {"formats": ["f"], "project": rec})
        self.respond(d, [{"match": "files.f", "stdout": dump(rec),
                          "stderr": "odin3: info: files.f: ignored: LOCATION\n"}])
        code, out = self.compare()
        self.assertEqual(code, 1)
        self.assertIn("ignored info line: path 'files.f' is not absolute", out)

    def test_record_equality_is_type_strict(self) -> None:
        rec = {**RECORD, "rules": [{"rule": "split", "subject": "$mem", "width": True,
                                    "depth": "max"},
                                   {"rule": "limit", "cell": "multiply", "count": 4}]}
        d = self.case("pos", {"formats": ["o3proj"], "project": rec})
        for field, value in (("width", 1), ("count", 4.0)):
            with self.subTest(field=field):
                dumped = absolute(rec)
                dumped["rules"][0 if field == "width" else 1][field] = value
                self.respond(d, [{"match": "o3proj", "stdout": json.dumps(dumped)}])
                code, out = self.compare()
                self.assertEqual(code, 1)
                self.assertIn("record rules", out)

    def test_nested_key_order(self) -> None:
        d = self.case("pos", {"formats": ["o3proj"], "project": RECORD})
        dumped = absolute(RECORD)
        dumped["files"] = [{"language": "verilog", "path": "{CASE}/src/top.v",
                            "library": "work"}]
        self.respond(d, [{"match": "o3proj", "stdout": json.dumps(dumped)}])
        code, out = self.compare()
        self.assertEqual(code, 1)
        self.assertIn("record files", out)


class CorpusCompareTest(unittest.TestCase):
    def test_entry_quoting(self) -> None:
        self.assertEqual(pf.script_word("a/b.f"), "a/b.f")
        self.assertEqual(pf.script_word("a b;c.f"), '"a b;c.f"')
        with self.assertRaises(pf.UsageError):
            pf.script_word('a"b')

    def test_frontend_kinds_are_error_kinds(self) -> None:
        self.assertLessEqual(pf.FRONTEND_KINDS | pf.PREPROCESS_KINDS, set(pf.ERROR_KINDS))
        self.assertEqual(pf.LANDED_FORMAT_PHASES, frozenset({pf.ODIN2_READER}))


if __name__ == "__main__":
    unittest.main()

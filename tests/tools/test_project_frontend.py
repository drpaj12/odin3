"""Tests for `project-fixtures compare --with-frontend --yosys Y --equiv-check E` (2D Task 26;
spec 14.4): the bridge on the positive phase-2 cases, over fake odin3, Yosys and equiv-check
scripts. Oracle drift (0.69 `ref.cmd` against 0.55) and a Yosys crash are reported, never failed."""

from __future__ import annotations

import contextlib
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest import mock

import memcap
import project_fixtures as pf

from . import helpers

# The fake odin3 answers the record dump with the case's record, and `write_rtlil FILE` by writing
# FILE.
FAKE_ODIN3 = """#!{python}
import json, os, sys
script = sys.argv[-1]
if "dump_project" in script:
    print(open({record!r}).read())
    sys.exit(0)
if os.environ.get("FAKE_ODIN3_RC"):
    print("odin3: error: boom", file=sys.stderr)
    sys.exit(int(os.environ["FAKE_ODIN3_RC"]))
for cmd in (c.strip() for c in script.split(";")):
    if cmd.startswith("write_rtlil "):
        open(cmd.split(None, 1)[1].strip('"'), "w").write("module \\\\top\\nend\\n")
"""

# Yosys writes $FAKE_A_BLIF for the back end that reads a.il, $FAKE_R55_BLIF for ref.cmd's run
# (a script that reads Verilog), and dies of $FAKE_R55_SIGNAL or exits $FAKE_R55_RC there.
FAKE_YOSYS = """#!{python}
import os, signal, sys
argv = sys.argv[1:]
script = argv[argv.index("-p") + 1]
if "read_rtlil" in script:
    text = os.environ.get("FAKE_A_BLIF", "REF")
else:
    if os.environ.get("FAKE_R55_SIGNAL"):
        os.kill(os.getpid(), int(os.environ["FAKE_R55_SIGNAL"]))
    if os.environ.get("FAKE_R55_RC"):
        sys.exit(int(os.environ["FAKE_R55_RC"]))
    text = os.environ.get("FAKE_R55_BLIF", "REF")
target = script.split("write_blif ")[-1].split()[-1].strip()
open(target, "w").write(text)
"""

FAKE_EQUIV = """#!{python}
import sys
a, b = (open(p).read() for p in sys.argv[1:3])
if a == b:
    print("equiv-check: equivalent")
    sys.exit(0)
print("equiv-check: NOT equivalent")
sys.exit(1)
"""

RECORD: dict[str, Any] = {
    **pf.empty_record(),
    "files": [{"path": "src/top.v", "language": "verilog", "library": "work"}],
}


def _exe(path: Path, text: str, **fields: str) -> Path:
    body = text.replace("{python}", sys.executable)
    for key, value in fields.items():
        body = body.replace("{" + key + "!r}", repr(value))
    path.write_text(body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class FrontendBridgeTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        self.root = self.dir / "corpus"
        self.case_dir = self.root / "pos"
        (self.case_dir / "src").mkdir(parents=True)
        (self.case_dir / "src" / "top.v").write_text("module top; endmodule\n")
        (self.case_dir / "ref.blif").write_text("REF")
        (self.case_dir / "ref.cmd").write_text(
            'mkdir -p out\n"$YOSYS" -q -p "read_verilog src/top.v; write_blif out/ref.blif"\n')
        exp = {"description": "pos", "features": ["verilog"], "phase": 2, "formats": ["o3proj"],
               "project": RECORD, "resolved": {"top": "work.top"}, "oracle": "yosys"}
        (self.case_dir / "expected.json").write_text(json.dumps(exp))
        record = json.loads(json.dumps(RECORD))
        record["files"][0]["path"] = str(self.case_dir / "src" / "top.v")
        record_file = self.dir / "record.json"
        record_file.write_text(json.dumps(record, indent=2))
        self.odin3 = _exe(self.dir / "odin3", FAKE_ODIN3, record=str(record_file))
        self.yosys = _exe(self.dir / "yosys", FAKE_YOSYS)
        self.equiv = _exe(self.dir / "equiv-check", FAKE_EQUIV)
        meminfo = self.dir / "meminfo"
        meminfo.write_text("MemTotal:       33554432 kB\n", encoding="ascii")
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.object(memcap, "MEMINFO", str(meminfo)))
        stack.enter_context(mock.patch.object(memcap, "SYSTEMD_RUN", "no-such-systemd-run"))
        self.addCleanup(stack.close)

    def compare(self, *extra: str, env: dict[str, str] | None = None) -> tuple[int, str]:
        argv = ["compare", "--odin3", str(self.odin3), "--root", str(self.root), "--with-frontend",
                "--yosys", str(self.yosys), "--equiv-check", str(self.equiv), *extra]
        with mock.patch.dict(os.environ, env or {}):
            code, out, err = helpers.run_main(pf.main, argv)
        return code, out + err

    def test_an_equivalent_case_is_ok(self) -> None:
        code, out = self.compare()
        self.assertEqual(code, 0, out)
        self.assertIn("ok     pos o3proj (bridge)", out)
        self.assertIn("bridge 1 ok, 0 failed, 0 oracle drift, 0 oracle crash, 0 skipped", out)

    def test_not_equivalent_while_0_55_reproduces_ref_blif_is_a_failure(self) -> None:
        code, out = self.compare(env={"FAKE_A_BLIF": "OTHER"})
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL   pos o3proj (bridge)", out)
        self.assertIn("Yosys 0.55 reproduces ref.blif", out)

    def test_0_55_disagreeing_with_the_recorded_0_69_is_oracle_drift(self) -> None:
        code, out = self.compare(env={"FAKE_A_BLIF": "OTHER", "FAKE_R55_BLIF": "OTHER"})
        self.assertEqual(code, 0, out)
        self.assertIn("drift  pos o3proj (bridge)", out)
        self.assertIn("oracle drift", out)
        self.assertIn("bridge 0 ok, 0 failed, 1 oracle drift", out)

    def test_a_ref_cmd_that_fails_under_0_55_is_oracle_drift_too(self) -> None:
        code, out = self.compare(env={"FAKE_A_BLIF": "OTHER", "FAKE_R55_RC": "1"})
        self.assertEqual(code, 0, out)
        self.assertIn("ref.cmd fails under Yosys 0.55", out)

    def test_a_yosys_signal_exit_is_an_oracle_crash_not_a_failure(self) -> None:
        code, out = self.compare(env={"FAKE_A_BLIF": "OTHER", "FAKE_R55_SIGNAL": "11"})
        self.assertEqual(code, 0, out)
        self.assertIn("ocrash pos o3proj (bridge)", out)
        self.assertIn("1 oracle crash", out)

    def test_odin3_failing_on_the_project_is_a_failure(self) -> None:
        code, out = self.compare(env={"FAKE_ODIN3_RC": "1"})
        self.assertEqual(code, 1, out)
        self.assertIn("odin3 exit 1: odin3: error: boom", out)

    def test_without_yosys_the_bridge_is_not_run(self) -> None:
        code, out, err = helpers.run_main(pf.main, [
            "compare", "--odin3", str(self.odin3), "--root", str(self.root), "--with-frontend"])
        self.assertEqual(code, 0, out + err)
        self.assertNotIn("(bridge)", out)
        self.assertIn("not run: --yosys and --equiv-check not given", out)

    def test_only_phase_2_verilog_cases_with_a_yosys_oracle_take_the_bridge(self) -> None:
        exp = json.loads((self.case_dir / "expected.json").read_text())
        self.assertTrue(pf.positive_frontend(exp))
        self.assertFalse(pf.positive_frontend({**exp, "phase": 5}))
        self.assertFalse(pf.positive_frontend({**exp, "oracle": "ghdl+yosys"}))
        self.assertFalse(pf.positive_frontend({**exp, "error": {"kind": "no_top"}}))
        blif = {**exp, "project": {**RECORD, "files": [{"language": "blif"}]}}
        self.assertFalse(pf.positive_frontend(blif))


if __name__ == "__main__":
    unittest.main()

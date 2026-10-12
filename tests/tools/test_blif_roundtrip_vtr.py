"""Tests for tools/blif-roundtrip/blif-roundtrip --vtr (3E spec §16.3; plan 3E Task 10).

The script runs over a fake ``odin3-blif-rt`` and a fake ``equiv-check`` (the real
``netlist-compare``). The fake driver copies its input to its output, and a ``# fake-rt: ...``
line in the golden changes that:

- ``refuse <text>``: log ``odin3: error: write_blif: --vtr: <text>``, write nothing, exit 1;
- ``drift``: append a blank line to every write, so W1 != W2 byte for byte (the netlist is the
  same);
- ``legalise``: escape every `%` as `%25` and every mid-line `\\` as `%5C` (VB-10 rule 1, as the
  writer does it) and log the ``names legalised`` info line;
- ``legalise-once``: the same, but leave a file that already holds a `%` unescaped, so W2 ==
  W1: a second write that forgot to escape, which only the one-step decode catches.

On a golden root, the script is a heavy run (plan 3E point (a)): those cases run a copy of the
script in a fake repository whose ``tools/vtr-qor/qor_heavy.py`` and ``tools/heavy-run`` log
their argv and environment.
"""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from . import helpers

SCRIPT = helpers.REPO_ROOT / "tools" / "blif-roundtrip" / "blif-roundtrip"
NC = helpers.REPO_ROOT / "tools" / "netlist-compare" / "netlist-compare"
TIMEOUT = 300
HEADER = "golden\ttool\twritten|refused\tclass\tidempotent\tcompare\tequiv\tverdict"

FAKE_RT = """\
#!{python}
import json, os, re, sys
args = sys.argv[1:]
log = os.environ.get("FAKE_RT_LOG")
if log:
    with open(log, "a", encoding="utf-8") as fh:
        fh.write(json.dumps(args) + "\\n")
paths = [a for i, a in enumerate(args) if not a.startswith("-") and args[i - 1] != "--techlib"]
src, dst = paths
text = open(src, encoding="utf-8").read()
marks = re.findall(r"^# fake-rt: (.*)$", text, re.M)
for mark in marks:
    if mark.startswith("refuse "):
        print("odin3: error: write_blif: --vtr: " + mark[len("refuse "):], file=sys.stderr)
        sys.exit(1)
out = text
if "drift" in marks:
    out += "\\n"
once = "legalise-once" in marks
if "legalise" in marks or (once and "%" not in text):
    lines = []
    for line in text.split("\\n"):
        if not line.startswith("#"):
            body, cont = (line[:-1], "\\\\") if line.endswith("\\\\") else (line, "")
            line = body.replace("%", "%25").replace("\\\\", "%5C") + cont
        lines.append(line)
    out = "\\n".join(lines)
    print("odin3: info: write_blif: --vtr: 1 names legalised (first: 'a' -> 'b')", file=sys.stderr)
open(dst, "w", encoding="utf-8").write(out)
"""

FAKE_EQUIV = """\
#!/usr/bin/env bash
echo "equiv-check: equivalent (fake)"
"""

FAKE_QOR_HEAVY = """\
import json, os, sys
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "qor.log"), "a", encoding="utf-8") as fh:
    fh.write(json.dumps(sys.argv[1:]) + "\\n")
with open(os.path.join(here, "rc.txt"), encoding="utf-8") as fh:
    sys.exit(int(fh.read()))
"""

FAKE_HEAVY_RUN = """\
#!{python}
import json, os, sys
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "heavy.log"), "w", encoding="utf-8") as fh:
    json.dump({{"argv": sys.argv[1:], "mark": os.environ.get("FAKE_MARK"),
               "ok": os.environ.get("ODIN3_HEAVY_OK")}}, fh)
"""


def golden(*body: str, marks: tuple[str, ...] = ()) -> str:
    """A small golden: top `m` with input a, clk and output y, then body lines."""
    lines = [*(f"# fake-rt: {m}" for m in marks), ".model m", ".inputs a clk", ".outputs y"]
    return "\n".join([*lines, *body, ".end"]) + "\n"


BUF = golden(".names a y", "1 1")
FE = golden(".latch a q fe clk 2", ".names q y", "1 1")


def write_exe(path: Path, text: str) -> None:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class VtrRoundTripTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="brt-vtr-test."))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.fixtures = self.tmp / "fixtures"
        self.fixtures.mkdir()
        self.rt = self.tmp / "fake-rt"
        write_exe(self.rt, FAKE_RT.format(python=sys.executable))
        self.equiv = self.tmp / "fake-equiv"
        write_exe(self.equiv, FAKE_EQUIV)
        self.rt_log = self.tmp / "rt.log"
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("ODIN3_HEAVY")}
        self.env["FAKE_RT_LOG"] = str(self.rt_log)

    def add(self, name: str, text: str, where: Path | None = None) -> Path:
        path = (where or self.fixtures) / name
        path.write_text(text, encoding="utf-8")
        return path

    def run_script(
        self, *target: str, script: Path = SCRIPT, env: dict[str, str] | None = None
    ) -> tuple[int, str, list[list[str]]]:
        """Runs the script --vtr; (exit code, stdout + stderr, report rows without the header)."""
        report = self.tmp / "report.tsv"
        report.unlink(missing_ok=True)
        argv = [str(script), "--vtr", "--rt", str(self.rt), "--nc", str(NC), "--equiv",
                str(self.equiv), "--out", str(self.tmp / "out"), "--report", str(report),
                *(target or ("--fixtures", str(self.fixtures)))]
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=TIMEOUT,
                              env=env or self.env, check=False)
        rows: list[list[str]] = []
        if report.is_file():
            lines = report.read_text(encoding="utf-8").splitlines()
            self.assertEqual(HEADER, lines[0])
            rows = [line.split("\t") for line in lines[1:]]
        return proc.returncode, proc.stdout + proc.stderr, rows

    def rt_inputs(self) -> list[str]:
        """The goldens odin3-blif-rt was given (W1 writes), in call order."""
        calls = [json.loads(line) for line in self.rt_log.read_text(encoding="utf-8").splitlines()]
        for call in calls:
            self.assertIn("--vtr", call)
        return [Path(c[-2]).name for c in calls if Path(c[-2]).parent == self.fixtures]

    def test_smallest_first(self) -> None:
        self.add("a_big.parmys.blif", golden(*[f".names a n{i}\n1 1" for i in range(9)],
                                             ".names a y", "1 1"))
        self.add("b_small.parmys.blif", BUF)
        self.add("c_mid.odin.blif", golden(".names a n", "1 1", ".names n y", "1 1"))
        self.add("d_other.blif", BUF)  # neither Parmys nor Odin II: skipped
        code, out, rows = self.run_script()
        self.assertEqual(0, code, out)
        order = ["b_small.parmys.blif", "c_mid.odin.blif", "a_big.parmys.blif"]
        self.assertEqual(order, self.rt_inputs())
        self.assertEqual(order, [r[0] for r in rows])
        self.assertEqual(["written", "-", "bytes", "identical", "-", "pass"], rows[0][2:])
        self.assertEqual(["written", "-", "bytes", "normalized", "equivalent", "pass"], rows[1][2:])
        self.assertIn("(1 other BLIFs skipped)", out)

    def test_w1_not_w2_fails(self) -> None:
        self.add("drift.parmys.blif", golden(".names a y", "1 1", marks=("drift",)))
        code, out, rows = self.run_script()
        self.assertEqual(1, code, out)
        self.assertEqual(["drift.parmys.blif", "parmys", "written", "-", "differ", "identical",
                          "-", "FAIL"], rows[0])
        self.assertIn("why: idempotence: differ", out)

    def test_renamed_compared_decoded(self) -> None:
        self.add("bs.parmys.blif", golden(".names a a\\b", "1 1", ".names a\\b y", "1 1",
                                          marks=("legalise",)))
        code, out, rows = self.run_script()
        self.assertEqual(0, code, out)
        self.assertEqual(["written", "-", "decoded", "identical", "-", "pass"], rows[0][2:])

    def test_second_write_not_escaped_fails(self) -> None:
        # W1 holds a%5Cb; W2 repeats it unescaped. The fixed-point decode would call them equal
        # (both decode to a\b); one decoding step of W2 gives a\b != a%5Cb, so it fails.
        self.add("once.parmys.blif", golden(".names a a\\b", "1 1", ".names a\\b y", "1 1",
                                            marks=("legalise-once",)))
        code, out, rows = self.run_script()
        self.assertEqual(1, code, out)
        self.assertEqual("differ", rows[0][4])
        self.assertEqual("FAIL", rows[0][7])

    def test_fe_parmys_not_refused_fails(self) -> None:
        self.add("neg.parmys.blif", FE)
        code, out, rows = self.run_script()
        self.assertEqual(1, code, out)
        self.assertEqual(["written", "-", "bytes"], rows[0][2:5])
        self.assertEqual("FAIL", rows[0][7])
        self.assertIn("an fe latch was written; it must be refused (E1)", out)

    def test_refused_e1_passes(self) -> None:
        text = ("neg.parmys.blif:4: $_DFF_N_ '$c4': falling-edge flip-flop; VPR accepts only "
                "rising-edge latches (lower inverts the clock)")
        self.add("neg.parmys.blif", f"# fake-rt: refuse {text}\n" + FE)
        code, out, rows = self.run_script()
        self.assertEqual(0, code, out)
        self.assertEqual(["neg.parmys.blif", "parmys", "refused", "E1", "-", "-", "-", "pass"],
                         rows[0])

    def test_fe_refused_without_e1_fails(self) -> None:
        text = "x.blif:4: $pow '$c4': not a netlist cell (run lower)"
        self.add("neg.parmys.blif", f"# fake-rt: refuse {text}\n" + FE)
        code, out, rows = self.run_script()
        self.assertEqual(1, code, out)
        self.assertEqual(["refused", "E5"], rows[0][2:4])
        self.assertIn("an fe latch must be refused with E1", out)

    def test_refusal_classes_by_tool(self) -> None:
        e7 = "x.blif:5: $sop '$c9': 7 inputs exceed K=6 (run abc, or pass --lut-k)"
        self.add("wide.odin.blif", f"# fake-rt: refuse {e7}\n" + BUF)
        self.add("wide.parmys.blif", f"# fake-rt: refuse {e7}\n" + BUF)
        self.add("odd.odin.blif", "# fake-rt: refuse something new\n" + BUF)
        code, out, rows = self.run_script()
        self.assertEqual(1, code, out)
        by_name = {r[0]: r for r in rows}
        self.assertEqual(["refused", "E7", "-", "-", "-", "pass"], by_name["wide.odin.blif"][2:])
        self.assertEqual(["refused", "E7"], by_name["wide.parmys.blif"][2:4])
        self.assertEqual("FAIL", by_name["wide.parmys.blif"][7])
        self.assertIn("refusal class E7 not expected for a parmys golden", out)
        self.assertEqual(["refused", "?"], by_name["odd.odin.blif"][2:4])
        self.assertEqual("FAIL", by_name["odd.odin.blif"][7])

    def test_odin_equiv_skips(self) -> None:
        self.add("two.odin.blif", golden(".inputs c2", ".latch a q re clk 2",
                                         ".latch a r re c2 2", ".names q r y", "11 1"))
        self.add("ram.odin.blif", golden(".subckt multiply a=a out=y", ".end",
                                         ".model multiply", ".inputs a", ".outputs out",
                                         ".blackbox"))
        code, out, rows = self.run_script()
        self.assertEqual(0, code, out)
        self.assertEqual({"two.odin.blif": "multiclock", "ram.odin.blif": "excluded"},
                         {r[0]: r[6] for r in rows})

    def test_equiv_failures_fail(self) -> None:
        # oracle 4: NOT-equivalent (exit 1) and an equiv-check error (exit 2) fail; a timeout is
        # listed and passes
        self.add("e.odin.blif", BUF)
        for rc, text, word, verdict in ((1, "NOT equivalent", "NOT-equivalent", "FAIL"),
                                        (2, "ABC exited with status 3", "error", "FAIL"),
                                        (2, "running ABC failed: timed out", "timeout", "pass")):
            with self.subTest(word=word):
                write_exe(self.equiv, f"#!/usr/bin/env bash\necho 'equiv-check: {text}'\n"
                          f"exit {rc}\n")
                code, out, rows = self.run_script()
                self.assertEqual(1 if verdict == "FAIL" else 0, code, out)
                self.assertEqual([word, verdict], rows[0][6:8])
                if verdict == "FAIL":
                    self.assertIn(f"why: equiv-check: {word}", out)

    def test_compare_timeout_fails_in_fixtures_mode(self) -> None:
        self.add("slow.parmys.blif", BUF)
        slow_nc = self.tmp / "slow-nc"
        write_exe(slow_nc, "#!/usr/bin/env bash\nexec sleep 30\n")
        argv = [str(SCRIPT), "--vtr", "-t", "1", "--rt", str(self.rt), "--nc", str(slow_nc),
                "--out", str(self.tmp / "out"), "--fixtures", str(self.fixtures)]
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=TIMEOUT,
                              env=self.env, check=False)
        self.assertEqual(1, proc.returncode, proc.stdout + proc.stderr)
        self.assertIn("why: netlist-compare: timeout", proc.stdout)

    def test_rt_error_fails(self) -> None:
        self.add("bad.parmys.blif", BUF)
        write_exe(self.rt, "#!/usr/bin/env bash\necho 'odin3: error: x: cannot open' >&2\nexit 2\n")
        code, out, rows = self.run_script()
        self.assertEqual(1, code, out)
        self.assertEqual(["error", "-"], rows[0][2:4])
        self.assertIn("odin3-blif-rt --vtr exited 2: odin3: error: x: cannot open", out)

    def test_excluded_not_run(self) -> None:
        self.add("LargeRam.odin.blif", golden(".names a y", "1 1", marks=("refuse x",)))
        self.add("ok.parmys.blif", BUF)
        code, out, rows = self.run_script()
        self.assertEqual(0, code, out)
        self.assertEqual(["ok.parmys.blif"], self.rt_inputs())
        self.assertIn(["LargeRam.odin.blif", "odin", "excluded", "-", "-", "-", "-", "excluded"],
                      rows)
        self.assertIn("excluded: 915 MB", out)
        self.assertIn("excluded 1,", out)

    def test_report_needs_vtr(self) -> None:
        proc = subprocess.run([str(SCRIPT), "--report", str(self.tmp / "r.tsv"), "--fixtures",
                               str(self.fixtures)], capture_output=True, text=True,
                              timeout=TIMEOUT, env=self.env, check=False)
        self.assertEqual(2, proc.returncode)


class HeavyShimTest(unittest.TestCase):
    """Point (a): `blif-roundtrip --vtr GOLDEN_ROOT` is a heavy run."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="brt-heavy-test."))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        repo = self.tmp / "repo"
        (repo / "tools" / "blif-roundtrip").mkdir(parents=True)
        (repo / "tools" / "vtr-qor").mkdir(parents=True)
        self.script = repo / "tools" / "blif-roundtrip" / "blif-roundtrip"
        shutil.copy2(SCRIPT, self.script)
        self.qor = repo / "tools" / "vtr-qor"
        (self.qor / "qor_heavy.py").write_text(FAKE_QOR_HEAVY, encoding="utf-8")
        self.heavy = repo / "tools" / "heavy-run"
        write_exe(self.heavy, FAKE_HEAVY_RUN.format(python=sys.executable))
        self.root = self.tmp / "golden"
        (self.root / "d").mkdir(parents=True)
        (self.root / "d" / "x.parmys.blif").write_text(BUF, encoding="utf-8")
        (self.root / "d" / "x.parmys.prov").write_text("status=ok\n", encoding="utf-8")
        self.rt = self.tmp / "fake-rt"
        write_exe(self.rt, FAKE_RT.format(python=sys.executable))
        self.env = {k: v for k, v in os.environ.items() if not k.startswith("ODIN3_HEAVY")}
        self.env["FAKE_MARK"] = "passed-through"

    def run_root(self, require_rc: int, heavy_ok: bool) -> subprocess.CompletedProcess[str]:
        (self.qor / "rc.txt").write_text(str(require_rc), encoding="utf-8")
        env = dict(self.env)
        if heavy_ok:
            env["ODIN3_HEAVY_OK"] = "1"
        self.argv = ["--vtr", "--rt", str(self.rt), "--nc", str(NC), "--out",
                     str(self.tmp / "out"), str(self.root)]
        return subprocess.run([str(self.script), *self.argv], capture_output=True, text=True,
                              timeout=TIMEOUT, env=env, check=False)

    def test_not_inside_with_ok_execs_heavy_run(self) -> None:
        proc = self.run_root(3, heavy_ok=True)
        self.assertEqual(0, proc.returncode, proc.stderr)
        self.assertEqual([["require-heavy"]], self.qor_calls())
        logged = json.loads((self.heavy.parent / "heavy.log").read_text(encoding="utf-8"))
        self.assertEqual([str(self.script), *self.argv], logged["argv"])
        self.assertEqual("passed-through", logged["mark"])
        self.assertEqual("1", logged["ok"])
        self.assertFalse((self.tmp / "out" / "report.tsv").exists())

    def test_not_inside_without_ok_exits_2(self) -> None:
        proc = self.run_root(3, heavy_ok=False)
        self.assertEqual(2, proc.returncode)
        self.assertIn("blif-roundtrip: heavy runs are the controller's: set ODIN3_HEAVY_OK=1",
                      proc.stderr)
        self.assertFalse((self.heavy.parent / "heavy.log").exists())

    def test_inside_runs(self) -> None:
        proc = self.run_root(0, heavy_ok=False)
        self.assertEqual(0, proc.returncode, proc.stdout + proc.stderr)
        self.assertFalse((self.heavy.parent / "heavy.log").exists())
        report = (self.tmp / "out" / "report.tsv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(["d/x.parmys.blif", "parmys", "written", "-", "bytes", "identical", "-",
                          "pass"], report[1].split("\t"))

    def test_compare_timeout_listed_on_golden_root(self) -> None:
        slow_nc = self.tmp / "slow-nc"
        write_exe(slow_nc, "#!/usr/bin/env bash\nexec sleep 30\n")
        (self.qor / "rc.txt").write_text("0", encoding="utf-8")
        proc = subprocess.run([str(self.script), "--vtr", "-t", "1", "--rt", str(self.rt),
                               "--nc", str(slow_nc), "--out", str(self.tmp / "out"),
                               str(self.root)], capture_output=True, text=True, timeout=TIMEOUT,
                              env=self.env, check=False)
        self.assertEqual(0, proc.returncode, proc.stdout + proc.stderr)
        report = (self.tmp / "out" / "report.tsv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(["timeout", "-", "pass"], report[1].split("\t")[5:])

    def test_cap_check_failed_exits_2(self) -> None:
        proc = self.run_root(2, heavy_ok=True)
        self.assertEqual(2, proc.returncode)
        self.assertFalse((self.heavy.parent / "heavy.log").exists())

    def test_fixtures_never_heavy(self) -> None:
        (self.qor / "rc.txt").write_text("3", encoding="utf-8")
        proc = subprocess.run([str(self.script), "--vtr", "--rt", str(self.rt), "--nc", str(NC),
                               "--out", str(self.tmp / "out"), "--fixtures", str(self.root)],
                              capture_output=True, text=True, timeout=TIMEOUT, env=self.env,
                              check=False)
        self.assertEqual(0, proc.returncode, proc.stdout + proc.stderr)
        self.assertEqual([], self.qor_calls())

    def qor_calls(self) -> list[list[str]]:
        log = self.qor / "qor.log"
        if not log.exists():
            return []
        return [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]


if __name__ == "__main__":
    unittest.main()

"""Tests for ``tools/blif-roundtrip/blif-roundtrip --vtr`` (3E plan Task 10, spec 16.3).

A fake ``odin3-blif-rt`` (a Python script driven by ``# fake:`` lines in its input) stands for
the writer; the real ``netlist-compare`` compares the small netlists. The heavy-run gate is tested
in a copy of the tool's tree with a fake ``qor_heavy.py`` and a fake ``heavy-run``.
"""

from __future__ import annotations

import os
import shutil
import stat
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

from . import helpers

TOOL = helpers.REPO_ROOT / "tools" / "blif-roundtrip" / "blif-roundtrip"
NC = helpers.REPO_ROOT / "tools" / "netlist-compare" / "netlist-compare"
SCRATCH = Path("/tmp/claude-1000")
HEADER = "golden\ttool\twritten|refused\terror class\tidempotent\tcompare\tequiv\tverdict"

# The fake writer. `# fake:` lines in the input: `refuse E1|E7|E5|E12|E2`, `renamed N`, `drift`
# (every write appends a line, so W2 != W1), `mutate` (W1 gets an extra cell: not the same
# netlist), `flip` (every write toggles that cell), `io` (exit 2). The output is the input, its
# `# fake:` lines kept.
FAKE_RT = r"""#!/usr/bin/env python3
import os, sys
args = sys.argv[1:]
libs = []
while "--techlib" in args:
    i = args.index("--techlib"); libs.append(args[i + 1]); del args[i:i + 2]
vtr = "--vtr" in args
if vtr: args.remove("--vtr")
src, dst = args
log = os.environ.get("FAKE_RT_LOG")
if log:
    with open(log, "a") as fh:
        fh.write(" ".join(sys.argv[1:]) + "\n")
text = open(src).read()
facts = [l[8:].split() for l in text.splitlines() if l.startswith("# fake: ")]
MESSAGES = {
    "E1": "$_DFF_N_ '$c1': falling-edge flip-flop; VPR accepts only rising-edge latches",
    "E1b": "$_DFFSR_PPP_ '$c1': asynchronous set/reset flip-flop; VPR has none",
    "E5": "$pow '$c1': not a netlist cell (run lower)",
    "E7": "$sop '$c1': 7 inputs exceed K=6 (run abc, or pass --lut-k)",
    "E2": "$_DLATCH_P_ '$c1': level-sensitive latch; VPR accepts only rising-edge latches",
    "E12": "m: net 'y' has 2 drivers ('$c4', '$c5')",
}
for f in facts:
    if f[0] == "io":
        sys.exit(2)
    if f[0] == "refuse":
        print(f"odin3: error: write_blif: --vtr: x.blif:1: {MESSAGES[f[1]]}", file=sys.stderr)
        sys.exit(1)
out = text
for f in facts:
    if f[0] == "renamed":
        msg = f"odin3: info: write_blif: --vtr: {f[1]} names legalised (first: 'a')"
        print(msg, file=sys.stderr)
    if f[0] == "drift":
        out += "# one more\n"
    if f[0] == "mutate" and "mutated" not in text:
        out = out.replace(".end", ".names extra\n1\n.end", 1) + "# mutated\n"
    if f[0] == "flip":
        out = out.replace(".names extra\n1\n", "") if ".names extra" in text else \
            out.replace(".end", ".names extra\n1\n.end", 1)
open(dst, "w").write(out)
"""

FAKE_EQUIV = r"""#!/usr/bin/env python3
import os, sys
with open(os.environ["FAKE_EQUIV_LOG"], "a") as fh:
    fh.write(" ".join(sys.argv[1:]) + "\n")
sys.exit(int(os.environ.get("FAKE_EQUIV_RC", "0")))
"""

LOGGING_NC = f"""#!/usr/bin/env bash
echo "$*" >> "$FAKE_NC_LOG"
exec "{NC}" "$@"
"""

MEMLIMIT_NC = """#!/usr/bin/env bash
echo "MemoryError" >&2
exit 1
"""

FAKE_QOR_HEAVY = r"""#!/usr/bin/env python3
import os, sys
with open(os.environ["FAKE_HEAVY_LOG"], "a") as fh:
    fh.write("qor_heavy " + " ".join(sys.argv[1:]) + "\n")
sys.exit(int(os.environ["FAKE_REQUIRE_RC"]))
"""

FAKE_HEAVY_RUN = r"""#!/usr/bin/env python3
import os, sys
with open(os.environ["FAKE_HEAVY_LOG"], "a") as fh:
    fh.write("heavy-run " + " ".join(sys.argv[1:]) + "\n")
    ok, mark = os.environ.get("ODIN3_HEAVY_OK", ""), os.environ.get("MARK", "")
    fh.write("env " + ok + " " + mark + "\n")
"""

LATCH_OK = ".latch d q re clk 2\n"
BODY = textwrap.dedent(
    """\
    .model top
    .inputs d clk
    .outputs q
    {cells}.end
    """
)


def blif(*fakes: str, cells: str = LATCH_OK, pad: int = 0) -> str:
    """A small netlist; the fake directives go in as comments, pad adds comment lines (size)."""
    head = "".join(f"# fake: {fake}\n" for fake in fakes)
    return head + BODY.format(cells=cells) + "# pad\n" * pad


def make_exec(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class RoundTripVtrBase(unittest.TestCase):
    def setUp(self) -> None:
        SCRATCH.mkdir(parents=True, exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=SCRATCH, prefix="brt-vtr-test-")
        self.tmp = Path(self._tmp.name)
        self.rt = make_exec(self.tmp / "fake-rt", FAKE_RT)
        self.root = self.tmp / "golden"
        self.root.mkdir()
        self.rt_log = self.tmp / "rt.log"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def add(self, name: str, text: str, ok: bool = True) -> Path:
        """A golden `<name>` (with its .prov) under the golden root."""
        path = self.root / name
        path.write_text(text, encoding="utf-8")
        prov = path.with_suffix(".prov")
        prov.write_text("status=ok\n" if ok else "status=failed\n", encoding="utf-8")
        return path

    def run_tool(
        self,
        *extra: str,
        tool: Path = TOOL,
        env: dict[str, str] | None = None,
        golden: bool = False,
        nc: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        args = [str(tool), "--vtr", "--rt", str(self.rt), "--nc", str(nc or NC), "--no-equiv"]
        args += ["--out", str(self.tmp / "out"), *extra]
        args += [str(self.root)] if golden else ["--fixtures", str(self.root)]
        full_env = dict(os.environ, FAKE_RT_LOG=str(self.rt_log), **(env or {}))
        return subprocess.run(
            args, capture_output=True, text=True, check=False, env=full_env, timeout=300
        )

    def rows(self) -> list[list[str]]:
        lines = (self.tmp / "out" / "report.tsv").read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], HEADER)
        return [line.split("\t") for line in lines[1:]]


class ReportTest(RoundTripVtrBase):
    def test_header_and_smallest_first(self) -> None:
        self.add("big.parmys.blif", blif(pad=30))
        self.add("small.parmys.blif", blif())
        self.add("mid.parmys.blif", blif(pad=5))
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = self.rows()
        self.assertEqual(
            [row[0] for row in rows], ["small.parmys.blif", "mid.parmys.blif", "big.parmys.blif"]
        )
        self.assertEqual(rows[0], ["small.parmys.blif", "parmys", "written", "-", "bytes",
                                   "identical", "-", "pass"])
        self.assertIn("RESULT: PASS", result.stdout)
        # the work order: each file's W1 then its W2, file by file, in that order
        writes = self.rt_log.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(writes), 6)
        self.assertIn("small.parmys.blif", writes[0])
        self.assertIn("big.parmys.blif", writes[4])
        self.assertTrue(all("--vtr" in line for line in writes))

    def test_golden_root_lists_only_status_ok(self) -> None:
        self.add("a.parmys.blif", blif())
        self.add("b.parmys.blif", blif(), ok=False)
        result = self.run_tool(golden=True, env={"FAKE_REQUIRE_RC": "0"})
        # a golden root needs the heavy gate (the real qor_heavy: not inside a heavy run)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_techlib_reaches_both_writes(self) -> None:
        lib = self.tmp / "x.o3lib"
        lib.write_text("", encoding="utf-8")
        self.add("a.parmys.blif", blif())
        result = self.run_tool("--techlib", str(lib))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for line in self.rt_log.read_text(encoding="utf-8").splitlines():
            self.assertIn(f"--techlib {lib}", line)

    def test_non_vtr_flags_are_usage_errors(self) -> None:
        result = subprocess.run(
            [str(TOOL), "--rt", str(self.rt), "--no-equiv", "--fixtures", str(self.root)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 2)


class IdempotenceTest(RoundTripVtrBase):
    def test_w1_differs_from_w2_fails(self) -> None:
        self.add("a.parmys.blif", blif("drift"))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(self.rows()[0][4], "FAIL")
        self.assertEqual(self.rows()[0][7], "fail:idempotent")

    def test_renamed_names_compare_decoded(self) -> None:
        """A write that legalised names is compared under the decode, and listed `decoded`."""
        self.add("a.parmys.blif", blif("renamed 3"))
        nc_log = self.tmp / "nc.log"
        nc = make_exec(self.tmp / "nc", LOGGING_NC)
        result = self.run_tool(env={"FAKE_NC_LOG": str(nc_log)}, nc=nc)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        row = self.rows()[0]
        self.assertEqual(row[4], "decoded")
        self.assertEqual(row[5], "normalized")
        calls = nc_log.read_text(encoding="utf-8").splitlines()
        self.assertTrue(all("--vtr-normalize" in call for call in calls), calls)

    def test_a_renamed_file_that_drifts_still_fails_when_the_decode_differs(self) -> None:
        self.add("a.parmys.blif", blif("renamed 1", "flip"))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.rows()[0][4], "FAIL")


class CompareTest(RoundTripVtrBase):
    def test_parmys_compares_plain_first_and_odin_normalized(self) -> None:
        self.add("p.parmys.blif", blif())
        self.add("o.odin.blif", blif(pad=2))
        nc_log = self.tmp / "nc.log"
        nc = make_exec(self.tmp / "nc", LOGGING_NC)
        result = self.run_tool(env={"FAKE_NC_LOG": str(nc_log)}, nc=nc)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = {row[1]: row for row in self.rows()}
        self.assertEqual(rows["parmys"][5], "identical")
        self.assertEqual(rows["odin"][5], "normalized")
        calls = nc_log.read_text(encoding="utf-8").splitlines()
        parmys_call = next(c for c in calls if "p.parmys" in c or "w1.blif" in c)
        odin_calls = [call for call in calls if "o.odin" in call]
        self.assertTrue(odin_calls and all("--vtr-normalize" in call for call in odin_calls), calls)
        self.assertNotIn("--vtr-normalize", parmys_call)

    def test_a_different_netlist_fails(self) -> None:
        self.add("a.parmys.blif", blif("mutate"))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(self.rows()[0][7], "fail:compare-different")

    def test_other_files_get_oracle_one_only(self) -> None:
        self.add("hand_x.blif", blif("mutate"))
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        row = self.rows()[0]
        self.assertEqual((row[1], row[5], row[7]), ("other", "-", "pass"))


class RefusalTest(RoundTripVtrBase):
    FE = ".latch d q fe clk 2\n"
    WIDE = ".names a b c d e f g q\n1111111 1\n"

    def test_a_refused_fe_file_passes_as_e1(self) -> None:
        self.add("fe.parmys.blif", blif("refuse E1", cells=self.FE))
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.rows()[0],
                         ["fe.parmys.blif", "parmys", "refused", "E1", "-", "-", "-", "pass"])

    def test_an_fe_file_that_is_not_refused_fails(self) -> None:
        self.add("fe.parmys.blif", blif(cells=self.FE))
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.rows()[0][7], "fail:fe-written")

    def test_fe_refused_with_another_class_fails(self) -> None:
        self.add("fe.parmys.blif", blif("refuse E5", cells=self.FE))
        self.assertEqual(self.run_tool().returncode, 1)
        self.assertEqual(self.rows()[0][7], "fail:fe-without-E1")

    def test_odin_wide_names_must_be_e7(self) -> None:
        self.add("w.odin.blif", blif("refuse E7", cells=self.WIDE))
        self.assertEqual(self.run_tool().returncode, 0)
        self.assertEqual(self.rows()[0][3], "E7")
        self.add("w2.odin.blif", blif(cells=self.WIDE, pad=3))
        self.assertEqual(self.run_tool().returncode, 1)
        self.assertEqual(self.rows()[1][7], "fail:wide-written")

    def test_multi_driver_odin_must_be_e12(self) -> None:
        self.add("multi_assignment.odin.blif", blif("refuse E12"))
        self.assertEqual(self.run_tool().returncode, 0)
        self.assertEqual(self.rows()[0][3], "E12")

    def test_async_and_word_cell_refusals_pass_on_parmys(self) -> None:
        self.add("dffsre.parmys.blif", blif("refuse E1b"))
        self.add("pow.parmys.blif", blif("refuse E5", pad=4))
        self.assertEqual(self.run_tool().returncode, 0)
        self.assertEqual([row[3] for row in self.rows()], ["E1b", "E5"])

    def test_a_class_a_golden_cannot_have_fails(self) -> None:
        self.add("x.parmys.blif", blif("refuse E2"))
        self.assertEqual(self.run_tool().returncode, 1)
        self.assertEqual(self.rows()[0][7], "fail:unexpected-E2")

    def test_other_files_may_be_refused_with_any_class(self) -> None:
        self.add("hand_x.blif", blif("refuse E2"))
        self.assertEqual(self.run_tool().returncode, 0)
        self.assertEqual(self.rows()[0][2:4], ["refused", "E2"])

    def test_an_io_failure_fails(self) -> None:
        self.add("x.parmys.blif", blif("io"))
        self.assertEqual(self.run_tool().returncode, 1)
        self.assertEqual(self.rows()[0][2:3], ["failed"])
        self.assertEqual(self.rows()[0][7], "fail:io")


class EquivTest(RoundTripVtrBase):
    def run_equiv(
        self, *netlists: tuple[str, str], rc: int = 0
    ) -> subprocess.CompletedProcess[str]:
        for name, text in netlists:
            self.add(name, text)
        equiv = make_exec(self.tmp / "fake-equiv", FAKE_EQUIV)
        self.equiv_log = self.tmp / "equiv.log"
        args = [str(TOOL), "--vtr", "--rt", str(self.rt), "--nc", str(NC), "--equiv", str(equiv),
                "--out", str(self.tmp / "out"), "--fixtures", str(self.root)]
        env = dict(os.environ, FAKE_RT_LOG=str(self.rt_log), FAKE_EQUIV_LOG=str(self.equiv_log),
                   FAKE_EQUIV_RC=str(rc))
        return subprocess.run(args, capture_output=True, text=True, check=False, env=env,
                              timeout=300)

    def test_odin_files_run_equiv_with_the_timeout(self) -> None:
        result = self.run_equiv(("a.odin.blif", blif()), ("p.parmys.blif", blif(pad=1)))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.equiv_log.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0].startswith("--allow-undriven --timeout 600 "), calls)
        self.assertIn("a.odin.blif", calls[0])
        self.assertEqual([row[6] for row in self.rows()], ["equivalent", "-"])

    def test_not_equivalent_fails(self) -> None:
        result = self.run_equiv(("a.odin.blif", blif()), rc=1)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.rows()[0][6:], ["NOTEQ", "fail:equiv-NOTEQ"])

    def test_a_timeout_is_listed_not_a_failure(self) -> None:
        result = self.run_equiv(("a.odin.blif", blif()), rc=4)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.rows()[0][6:], ["timeout", "pass"])

    def test_ram_multiply_and_multiclock_are_skipped(self) -> None:
        mult = ".subckt multiply a[0]=d b[0]=d out[0]=q\n"
        result = self.run_equiv(
            ("m.odin.blif", blif(cells=mult)),
            ("c.odin.blif", blif(cells=".latch d q re clk 2\n.latch d q2 re clk2 2\n", pad=9)),
        )
        # the fake netlists are not valid hard blocks; only the equiv column is under test
        self.assertEqual([row[6] for row in self.rows()], ["skipped", "multiclock"], result.stdout)
        self.assertFalse(self.equiv_log.exists())


class HeavyGateTest(RoundTripVtrBase):
    """On a golden root: point (a), in a copy of the tool's tree with fakes for the two helpers."""

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.tmp / "repo"
        for sub in ("blif-roundtrip", "vtr-qor", "netlist-compare", "equiv-check"):
            (self.repo / "tools" / sub).mkdir(parents=True)
        self.script = self.repo / "tools" / "blif-roundtrip" / "blif-roundtrip"
        shutil.copy2(TOOL, self.script)
        make_exec(self.repo / "tools" / "vtr-qor" / "qor_heavy.py", FAKE_QOR_HEAVY)
        make_exec(self.repo / "tools" / "heavy-run", FAKE_HEAVY_RUN)
        self.log = self.tmp / "heavy.log"
        self.add("a.parmys.blif", blif())

    def run_golden(self, require_rc: int, ok: bool) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ, FAKE_HEAVY_LOG=str(self.log), FAKE_REQUIRE_RC=str(require_rc),
                   FAKE_RT_LOG=str(self.rt_log), MARK="same")
        env.pop("ODIN3_HEAVY_OK", None)
        if ok:
            env["ODIN3_HEAVY_OK"] = "1"
        args = [str(self.script), "--vtr", "--rt", str(self.rt), "--nc", str(NC), "--no-equiv",
                "--out", str(self.tmp / "out"), str(self.root)]
        return subprocess.run(args, capture_output=True, text=True, check=False, env=env,
                              timeout=300)

    def run_memlimit(self, name: str) -> subprocess.CompletedProcess[str]:
        """A golden-mode run whose netlist-compare hits the memory limit on the odin file `name`."""
        self.add("a.parmys.blif", blif(), ok=False)  # not a golden: setUp's file stays out
        self.add(name, blif())
        nc = make_exec(self.tmp / "memlimit-nc", MEMLIMIT_NC)
        env = dict(os.environ, FAKE_HEAVY_LOG=str(self.log), FAKE_REQUIRE_RC="0",
                   FAKE_RT_LOG=str(self.rt_log))
        args = [str(self.script), "--vtr", "--rt", str(self.rt), "--nc", str(nc), "--no-equiv",
                "--out", str(self.tmp / "out"), str(self.root)]
        return subprocess.run(args, capture_output=True, text=True, check=False, env=env,
                              timeout=300)

    def test_a_listed_memlimit_is_capped_not_pass(self) -> None:
        result = self.run_memlimit("LargeRam.odin.blif")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        row = next(r for r in self.rows() if r[0] == "LargeRam.odin.blif")
        self.assertEqual(row[5:], ["memlimit", "-", "capped (listed)"])
        self.assertIn("RESULT: PASS (1 unverified)", result.stdout)

    def test_an_unlisted_memlimit_fails(self) -> None:
        result = self.run_memlimit("Other.odin.blif")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        row = next(r for r in self.rows() if r[0] == "Other.odin.blif")
        self.assertEqual(row[7], "fail:compare-memlimit")

    def test_inside_a_heavy_run_it_runs(self) -> None:
        result = self.run_golden(0, ok=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.log.read_text(encoding="utf-8"), "qor_heavy require-heavy\n")
        self.assertEqual(self.rows()[0][7], "pass")

    def test_outside_with_the_variable_it_execs_heavy_run_with_the_environment(self) -> None:
        result = self.run_golden(3, ok=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = self.log.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines[0], "qor_heavy require-heavy")
        argv = lines[1].split(" ")
        self.assertEqual(argv[0:2], ["heavy-run", str(self.script)])
        self.assertEqual(argv[2], "--vtr")
        self.assertEqual(argv[-1], str(self.root))
        self.assertEqual(lines[2], "env 1 same")
        self.assertFalse((self.tmp / "out" / "report.tsv").exists())

    def test_outside_without_the_variable_it_exits_2(self) -> None:
        result = self.run_golden(3, ok=False)
        self.assertEqual(result.returncode, 2)
        self.assertIn("heavy runs are the controller's: set ODIN3_HEAVY_OK=1", result.stderr)
        self.assertNotIn("heavy-run", self.log.read_text(encoding="utf-8"))

    def test_a_failed_cap_check_exits_2(self) -> None:
        result = self.run_golden(2, ok=True)
        self.assertEqual(result.returncode, 2)
        self.assertNotIn("heavy-run", self.log.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()

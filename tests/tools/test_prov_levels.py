"""Tests for the provenance depth (--prov-level, --prov-budget, prov_estimate): docs/specs/
2026-11-13-prov-levels.md. Every test needs ODIN3_CLI (CTest python_tools); none needs VTR."""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import odin3_flow as flow

from .helpers import run_main

ROOT = Path(__file__).resolve().parents[2]
ODIN3 = os.environ.get("ODIN3_CLI", "")
MICRO = ROOT / "tests" / "micro" / "verilog" / "micro"
MUX16 = MICRO / "bm_DL_16_1_mux.v"
ALU = MICRO / "bm_DL_74381_ALU.v"
needs_odin3 = unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools)")
ESTIMATE_RE = re.compile(r"proc: provenance depth (\d) \(\w+\): estimated (\d+) records, "
                         r"(\d+) edges, (\d+) bytes \(name map (\d+), step files (\d+)\)")
DEPTHS = (0, 1, 2, 3, 4)
FACTOR = 2.0  # decision 4: the estimate is within 2x of the real size (name map, these micros)
STEP_FACTOR = 10.0  # the step files are the least predictable measure (the calibration doc)
HASH_NAME = re.compile(rb"\bh[0-9a-z]{8,12}\b")


class Sizes:
    """What one ``odin3-flow --no-vpr --prov-level`` run wrote."""

    def __init__(self, out: Path, code: int, stderr: str) -> None:
        self.code = code
        self.stderr = stderr
        self.out = out
        names = sorted((out / "odin3").glob("*.blif.names"))
        self.names_text = names[0].read_text() if names else ""
        self.map_bytes = names[0].stat().st_size if names else 0
        steps = out / "prov"
        self.step_bytes = sum(p.stat().st_size for p in steps.rglob("*") if p.is_file()) \
            if steps.is_dir() else 0
        summary = out / "summary.json"
        self.luts = json.loads(summary.read_text())["luts"] if summary.is_file() else -1
        blifs = sorted((out / "odin3").glob("*.blif"))
        self.blif = blifs[0].read_bytes() if blifs else b""
        log = out / "odin3" / "odin3.log"
        self.log = log.read_text(errors="replace") if log.is_file() else ""

    @property
    def total(self) -> int:
        return self.map_bytes + self.step_bytes

    @property
    def has_v2_map(self) -> bool:
        return self.names_text.startswith("# odin3-names 2 ")

    def blif_renamed(self) -> bytes:
        """The BLIF with its hash names numbered by first use: the structure, not the names."""
        seen: dict[bytes, bytes] = {}
        return HASH_NAME.sub(lambda m: seen.setdefault(m.group(0), b"n%d" % len(seen)), self.blif)


class ProvCase(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.runs = 0

    def flow(self, design: Path, *extra: str) -> Sizes:
        self.runs += 1
        out = self.tmp / f"run{self.runs}"
        env = {k: v for k, v in os.environ.items() if k != "VTR_ROOT"}
        argv = [str(design), "--top", design.stem, "--no-vpr", "--odin3", ODIN3, "--out",
                str(out), *extra]
        with mock.patch.dict(os.environ, env, clear=True):
            code, _, err = run_main(flow.main, argv)
        return Sizes(out, code, err)

    def odin3(self, *argv: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([ODIN3, *argv], capture_output=True, text=True, cwd=self.tmp,
                              check=False, timeout=120)


@needs_odin3
class SameNetlist(ProvCase):
    def runs_at_all_depths(self, design: Path) -> dict[int, Sizes]:
        runs = {d: self.flow(design, "--prov-level", str(d)) for d in DEPTHS}
        for depth, run in runs.items():
            self.assertEqual(run.code, 0, f"depth {depth}: {run.stderr}")
        return runs

    def test_luts_identical_and_sizes_monotone(self) -> None:
        for design in (MUX16, ALU):
            runs = self.runs_at_all_depths(design)
            self.assertEqual({r.luts for r in runs.values()}, {runs[0].luts}, design.name)
            self.assertGreater(runs[0].luts, 0)
            # depth 1 shares one record per abc run, so its hash names differ from depth 2's; the
            # structure does not, and depths 2 to 4 give the same bytes
            self.assertEqual({runs[d].blif_renamed() for d in (1, 2, 3, 4)},
                             {runs[1].blif_renamed()}, design.name)
            self.assertEqual({runs[d].blif for d in (2, 3, 4)}, {runs[2].blif}, design.name)
            self.assertEqual(runs[0].total, 0)
            totals = [runs[d].total for d in (1, 2, 3, 4)]
            # depth 1 shares one bulk record per abc run, which is not always smaller than the
            # per-LUT records of depth 2 (a few LUTs, many old cells): 5% slack there only
            self.assertLessEqual(totals[0], totals[1] * 1.05, f"{design.name}: sizes {totals}")
            self.assertEqual(totals[1:], sorted(totals[1:]), f"{design.name}: sizes {totals}")
            self.assertGreater(totals[0], 0)
            self.assertEqual([runs[d].step_bytes for d in (0, 1, 2, 3)], [0, 0, 0, 0])
            self.assertGreater(runs[4].step_bytes, 0)

    def test_depth_0_writes_no_map(self) -> None:
        run = self.flow(MUX16, "--prov-level", "0")
        self.assertFalse(run.has_v2_map)
        self.assertEqual(run.total, 0)
        self.assertEqual(run.code, 0, run.stderr)

    def test_default_is_unchanged(self) -> None:
        plain = self.flow(MUX16)
        self.assertFalse(plain.has_v2_map)
        self.assertEqual(plain.total, 0)
        self.assertNotIn("provenance depth", plain.log)
        self.assertEqual(plain.luts, self.flow(MUX16, "--prov-level", "2").luts)

    def test_bulk_depths_carry_a_shared_lut_record_without_a_warning(self) -> None:
        run = self.flow(MUX16, "--prov-level", "1")
        names = next((run.out / "odin3").glob("*.names")).read_text()
        self.assertIn("lossy=bulk", names)
        self.assertNotIn("share one record", run.log)
        self.assertNotIn("lossy=bulk", next(
            (self.flow(MUX16, "--prov-level", "2").out / "odin3").glob("*.names")).read_text())


@needs_odin3
class EstimateAccuracy(ProvCase):
    def estimate(self, design: Path, depth: int) -> tuple[int, int, int, int]:
        run = self.flow(design, "--prov-level", str(depth))
        match = ESTIMATE_RE.search(run.log)
        self.assertIsNotNone(match, run.log[-400:])
        assert match is not None
        records, edges, _total, map_bytes, step_bytes = (int(g) for g in match.groups()[1:3] +
                                                         match.groups()[3:])
        del records, edges
        return map_bytes, step_bytes, run.map_bytes, run.step_bytes

    def test_estimate_within_2x_on_two_micros(self) -> None:
        for design in (MUX16, ALU):
            for depth in (1, 2, 3, 4):
                est_map, est_steps, real_map, real_steps = self.estimate(design, depth)
                self.assertLessEqual(max(est_map, real_map) / min(est_map, real_map), FACTOR,
                                     f"{design.name} depth {depth} map {est_map} vs {real_map}")
                if depth == 4:
                    self.assertLessEqual(max(est_steps, real_steps) / min(est_steps, real_steps),
                                         STEP_FACTOR,
                                         f"{design.name} steps {est_steps} vs {real_steps}")
                else:
                    self.assertEqual((est_steps, real_steps), (0, 0))


@needs_odin3
class BudgetAndOptions(ProvCase):
    def test_fixed_depth_over_budget_drops_with_a_warning(self) -> None:
        run = self.flow(MUX16, "--prov-level", "4", "--prov-budget", "1")
        self.assertEqual(run.code, 0, run.stderr)
        self.assertRegex(run.log, r"provenance depth 4 \(full\) is estimated at \d+ bytes, over "
                                  r"the budget of 1 bytes; using depth 0 \(off\)")
        self.assertFalse(run.has_v2_map)
        self.assertEqual(run.step_bytes, 0)

    def test_budget_picks_the_highest_depth_that_fits(self) -> None:
        sizes = {d: self.flow(MUX16, "--prov-level", str(d)).total for d in (1, 2, 3, 4)}
        budget = sizes[2] * 3 // 2  # room for depth 2 with its own estimate's margin
        run = self.flow(MUX16, "--prov-level", "auto", "--prov-budget", str(budget))
        depth = ESTIMATE_RE.search(run.log)
        self.assertIsNotNone(depth, run.log[-400:])
        assert depth is not None
        picked = int(depth.group(1))
        self.assertIn(picked, (1, 2, 3, 4))
        self.assertLessEqual(int(depth.group(4)), budget)
        if picked < 4:
            # the next depth up is over the budget by the estimate
            est = self.odin3("--prov-level", str(picked + 1), "-p",
                             f"read_verilog --nolatches {MUX16}; proc")
            nxt = ESTIMATE_RE.search(est.stderr + est.stdout)
            self.assertIsNotNone(nxt)
            assert nxt is not None
            self.assertGreater(int(nxt.group(4)), budget)

    def test_budget_alone_is_auto(self) -> None:
        run = self.flow(MUX16, "--prov-budget", "1G")
        self.assertIn("proc: provenance depth 4 (full)", run.log)

    def test_bad_values_are_option_errors(self) -> None:
        for argv in (["--prov-level", "5"], ["--prov-level", "x"], ["--prov-budget", "0"],
                     ["--prov-budget", "1T"], ["--prov-level"]):
            proc = self.odin3(*argv, "-p", "stats")
            self.assertNotEqual(proc.returncode, 0, argv)

    def test_prov_estimate_pass_prints_every_depth(self) -> None:
        proc = self.odin3("-p", f"read_verilog --nolatches {MUX16}; proc; prov_estimate")
        text = proc.stderr + proc.stdout
        self.assertEqual(proc.returncode, 0, text)
        for depth in DEPTHS:
            self.assertRegex(text, rf"prov_estimate: depth {depth} \(\w+\): \d+ records")
        self.assertNotIn("provenance depth", text.split("prov_estimate: design")[0])


@needs_odin3
class Precedence(ProvCase):
    def script(self, abc_extra: str, write_extra: str) -> str:
        return (f"read_verilog --nolatches {MUX16}; proc; opt; flatten; opt -full; wreduce; "
                f"lower; abc --lut 6 {abc_extra}; write_blif --vtr --lut-k 6 --name-style hash "
                f"{write_extra} {self.tmp / 'out.blif'}")

    def names(self) -> str:
        return (self.tmp / "out.blif.names").read_text()

    def test_explicit_prov_map_wins_over_depth_0(self) -> None:
        proc = self.odin3("--prov-level", "0", "-p", self.script("", "--prov-map"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertTrue((self.tmp / "out.blif.names").is_file())

    def test_depth_asks_for_the_map_with_hash_names(self) -> None:
        proc = self.odin3("--prov-level", "2", "-p", self.script("", ""))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("# odin3-names 2 ", self.names().splitlines()[0])
        self.assertIn("ast=refs", self.names().splitlines()[0])

    def test_depth_0_and_no_option_write_no_map(self) -> None:
        for argv in (["--prov-level", "0"], []):
            proc = self.odin3(*argv, "-p", self.script("", ""))
            self.assertEqual(proc.returncode, 0, proc.stderr)
            names = self.tmp / "out.blif.names"  # hash names alone write a version 1 map
            self.assertFalse(names.is_file() and "odin3-names 2" in names.read_text(), argv)

    def test_explicit_prov_cut_wins_over_depth_2(self) -> None:
        self.assertNotIn("cut=", self.run_and_names("2", ""))
        self.assertIn("cut=", self.run_and_names("2", "--prov-cut on"))
        self.assertNotIn("cut=", self.run_and_names("4", "--prov-cut off"))
        self.assertIn("cut=", self.run_and_names("3", ""))

    def test_explicit_prov_edges_wins_over_depth_1(self) -> None:
        self.assertIn("lossy=bulk", self.run_and_names("1", ""))
        self.assertNotIn("lossy=bulk", self.run_and_names("1", "--prov-edges unlimited"))

    def run_and_names(self, depth: str, abc_extra: str) -> str:
        out = self.tmp / "out.blif.names"
        out.unlink(missing_ok=True)
        proc = self.odin3("--prov-level", depth, "-p", self.script(abc_extra, ""))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return out.read_text()


@needs_odin3
class ProjectKeys(ProvCase):
    def project(self, *lines: str) -> Path:
        path = self.tmp / "p.o3proj"
        path.write_text("\n".join([f"file verilog {MUX16}", *lines]) + "\n")
        return path

    def test_keys_are_dumped_and_the_command_line_wins(self) -> None:
        proj = self.project("prov_level 3", "prov_budget 2M")
        proc = self.odin3("--dump-project", "--project", str(proj))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        dump = json.loads(proc.stdout)
        self.assertEqual(dump["prov_level"], 3)
        self.assertEqual(dump["prov_budget"], 2 * 1024 * 1024)
        script = f"read_project {proj}; proc"
        from_project = self.odin3("-p", script)
        self.assertIn("proc: provenance depth 3 (cut)", from_project.stderr + from_project.stdout)
        cli = self.odin3("--prov-level", "1", "-p", script)
        self.assertIn("proc: provenance depth 1 (statement)", cli.stderr + cli.stdout)

    def test_auto_and_absence(self) -> None:
        proc = self.odin3("--dump-project", "--project", str(self.project("prov_level auto")))
        self.assertEqual(json.loads(proc.stdout)["prov_level"], "auto")
        proc = self.odin3("--dump-project", "--project", str(self.project()))
        self.assertNotIn("prov_level", json.loads(proc.stdout))
        self.assertNotIn("prov_budget", json.loads(proc.stdout))

    def test_bad_keys_are_errors(self) -> None:
        for line in ("prov_level 7", "prov_level", "prov_budget 0", "prov_budget 5T"):
            proc = self.odin3("--dump-project", "--project", str(self.project(line)))
            self.assertNotEqual(proc.returncode, 0, line)
        for twice in (("prov_level 1", "prov_level 2"), ("prov_budget 1M", "prov_budget 2M")):
            proc = self.odin3("--dump-project", "--project", str(self.project(*twice)))
            self.assertNotEqual(proc.returncode, 0, twice)


if __name__ == "__main__":
    unittest.main()

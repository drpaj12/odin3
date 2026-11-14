"""Tests for the timing hints (docs/specs/2026-11-14-timing-hints.md): the back-annotator, the hint
keys across runs, the abc hook, `odin3-flow --hints` and `--two-pass`. Tests that run Odin III need
ODIN3_CLI (CTest python_tools); the `--two-pass` test also needs VTR_ROOT."""

from __future__ import annotations

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import odin3_flow as flow
import timing_hints as th
import two_pass

from .helpers import run_main

ODIN3 = os.environ.get("ODIN3_CLI", "")
VTR_ROOT = os.environ.get("VTR_ROOT", "")
DEMO = Path(__file__).resolve().parent / "fixtures" / "hint_demo.v"  # registers and logic
needs_odin3 = unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools)")
UID = re.compile(r"uid=(\d+)")
AND11 = ("module top(input a,b,c,d,e,f,g,h,i,j,k, output y);\n"
         "  assign y = a&b&c&d&e&f&g&h&i&j&k;\nendmodule\n")
HEAD = "odin3-timing-hints\t1\nmeta\tcpd_ps\t1000\nmeta\tdepth\t2\nmeta\tslack_min_ps\t0\n"


def luts_of(blif: Path) -> list[str]:
    return sorted(th.read_blif(blif).luts)


def fake_net_timing(path: Path, atoms: list[str], slack_ps: dict[str, float]) -> None:
    """A VPR report_net_timing.csv with one row per LUT atom (seconds in the file)."""
    lines = ["src_pin_name,src_pin_slack,sinks"]
    for atom in atoms:
        lines.append(f"{atom}.out[0],{slack_ps[atom] * 1e-12:.6e},")
    path.write_text("\n".join(lines) + "\n")


class Case(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)
        self.runs = 0

    def flow(self, design: Path, *extra: str, env: dict[str, str] | None = None) -> Path:
        """``odin3-flow --no-vpr`` into a fresh directory; the output directory."""
        self.runs += 1
        out = self.tmp / f"run{self.runs}"
        keep = {k: v for k, v in os.environ.items() if k != "VTR_ROOT"}
        keep.update(env or {})
        argv = [str(design), "--top", design.stem, "--no-vpr", "--odin3", ODIN3, "--out",
                str(out), *extra]
        with mock.patch.dict(os.environ, keep, clear=True):
            code, _, err = run_main(flow.main, argv)
        self.assertEqual(code, 0, err)
        return out

    def odin3(self, script: str, *opts: str, env: dict[str, str] | None = None,
              top: str = "top") -> subprocess.CompletedProcess[str]:
        return subprocess.run([ODIN3, *opts, "--top", top, "-p", script], capture_output=True,
                              text=True, cwd=self.tmp, check=False, timeout=120,
                              env={**os.environ, **(env or {})})


class Annotator(Case):
    """The back-annotation on a real name map and a made-up routed net timing report."""

    def prepare(self, design: Path = DEMO) -> tuple[Path, Path, Path, dict[str, float]]:
        out = self.flow(design, "--prov-level", "3", env={"ODIN3_PROV_UID": "1"})
        blif = out / "odin3" / f"{design.stem}.blif"
        atoms = luts_of(blif)
        self.assertGreater(len(atoms), 3)
        slack = {a: 100.0 + 37.0 * i for i, a in enumerate(atoms)}  # ps, all different
        csv = self.tmp / "net.csv"
        fake_net_timing(csv, atoms, slack)
        return blif, blif.with_name(blif.name + ".names"), csv, slack

    @needs_odin3
    def test_hints_are_relative_to_the_worst_and_keyed_by_uid(self) -> None:
        blif, names, csv, slack = self.prepare()
        text, stats, hints = th.build(blif, names, [csv], [2.0])
        lines = text.splitlines()
        self.assertEqual(lines[0], "odin3-timing-hints\t1")
        self.assertIn("meta\tslack_min_ps\t0", lines)
        self.assertIn("meta\tcpd_ps\t2000.0", lines)
        self.assertGreater(stats.cells, 0)
        self.assertEqual(len([ln for ln in lines if ln.startswith("cell\t")]), stats.cells)
        self.assertEqual(min(h.slack_ps for h in hints), 0.0)  # the worst LUT has relative slack 0
        self.assertTrue(all(0.0 <= h.crit <= 1.0 for h in hints))
        uids = {int(m.group(1)) for m in UID.finditer(names.read_text())}
        self.assertTrue({h.uid for h in hints} <= uids)
        text2 = th.build(blif, names, [csv], [2.0])[0]
        self.assertEqual(text, text2)  # fixed tie seed: the same file twice

    @needs_odin3
    def test_worst_over_seeds_and_cut_options(self) -> None:
        blif, names, csv, slack = self.prepare(DEMO)
        atoms = sorted(slack)
        other = self.tmp / "net2.csv"
        fake_net_timing(other, atoms, {a: slack[a] for a in reversed(atoms)} | {})
        one = th.build(blif, names, [csv], [2.0])[2]
        two = th.build(blif, names, [csv, other], [2.0, 2.0])[2]
        by_one = {h.uid: h.slack_ps for h in one}
        for hint in two:  # the worst over two seeds is never above the first seed's
            self.assertLessEqual(hint.slack_ps, by_one.get(hint.uid, hint.slack_ps))
        text = th.build(blif, names, [csv, other], [2.0, 4.0])[0]
        self.assertIn("meta\tcpd_ps\t3000.0", text)  # the mean of the seeds' CPDs
        self.assertIn("meta\tseeds\t2", text)
        wide = th.build(blif, names, [csv], [2.0], cut_mode="all", max_cells=0)[1]
        self.assertGreaterEqual(wide.cells, th.build(blif, names, [csv], [2.0])[1].cells)

    @needs_odin3
    def test_errors(self) -> None:
        blif, names, csv, _ = self.prepare(DEMO)
        with self.assertRaisesRegex(th.HintError, "one --cpd-ns per"):
            th.build(blif, names, [csv], [])
        empty = self.tmp / "empty.csv"
        empty.write_text("src_pin_name,src_pin_slack,sinks\n")
        with self.assertRaisesRegex(th.HintError, "no pin slack"):
            th.build(blif, names, [empty], [1.0])
        # a name map without uid flags (ODIN3_PROV_UID unset) gives no key
        out = self.flow(DEMO, "--prov-level", "3")
        nouid = out / "odin3" / f"{DEMO.stem}.blif"
        atoms = luts_of(nouid)
        fake_net_timing(csv, atoms, {a: 10.0 for a in atoms})
        with self.assertRaisesRegex(th.HintError, "no LUT record gave a hint key"):
            th.build(nouid, nouid.with_name(nouid.name + ".names"), [csv], [1.0])

    def test_depth_of_a_small_blif(self) -> None:
        path = self.tmp / "d.blif"
        path.write_text(".model m\n.inputs a b c d\n.outputs y\n"
                        ".names a b n1\n11 1\n.names n1 c n2\n11 1\n.names n2 d y\n11 1\n"
                        ".names y z\n1 1\n.end\n")
        blif = th.read_blif(path)
        self.assertEqual(blif.depth["n1"], 1)
        self.assertEqual(blif.depth["y"], 3)
        self.assertEqual(blif.depth["z"], 3)  # a one-input .names costs nothing
        self.assertEqual(blif.max_depth, 3)
        self.assertEqual(sorted(blif.luts), ["n1", "n2", "y"])

    def test_net_timing_reader_takes_the_worst_pin(self) -> None:
        csv = self.tmp / "n.csv"
        csv.write_text("src_pin_name,src_pin_slack,sinks\n"
                       "u.out[0],5e-10,\"v.in[0],3e-10,1e-10;w.in[1],9e-10,1e-10\"\n")
        seed = th.read_net_timing(csv, 1.0)
        self.assertAlmostEqual(seed.rel_ps["v"], 0.0)  # 300 ps is the worst
        self.assertAlmostEqual(seed.rel_ps["u"], 200.0)
        self.assertAlmostEqual(seed.rel_ps["w"], 600.0)
        self.assertEqual(seed.pin_ps["v"][0][1], "v.in[0]")


class Keys(Case):
    """The hint keys are the same across runs, and `read_hints` does not shift them."""

    def uids(self, out: Path, design: Path) -> list[str]:
        return UID.findall((out / "odin3" / f"{design.stem}.blif.names").read_text())

    @needs_odin3
    def test_uids_repeat_and_read_hints_does_not_shift_them(self) -> None:
        env = {"ODIN3_PROV_UID": "1"}
        first = self.flow(DEMO, "--prov-level", "3", env=env)
        second = self.flow(DEMO, "--prov-level", "3", env=env)
        a, b = self.uids(first, DEMO), self.uids(second, DEMO)
        self.assertGreater(len(a), 10)
        self.assertEqual(a, b)
        blif = first / "odin3" / f"{DEMO.stem}.blif"
        names = blif.with_name(blif.name + ".names")
        csv = self.tmp / "net.csv"
        atoms = luts_of(blif)
        fake_net_timing(csv, atoms, {x: 50.0 * i for i, x in enumerate(atoms)})
        hints = self.tmp / "h.tsv"
        hints.write_text(th.build(blif, names, [csv], [2.0])[0])
        third = self.flow(DEMO, "--prov-level", "3", "--hints", str(hints), env=env)
        self.assertEqual(a, self.uids(third, DEMO))
        log = (third / "odin3" / "odin3.log").read_text()
        match = re.search(r"abc: timing hints \(.*?\): (\d+) of (\d+) cell hints match a gate", log)
        self.assertIsNotNone(match, log)
        assert match is not None
        self.assertEqual(match.group(1), match.group(2))  # every hint found its gate
        self.assertGreater(int(match.group(1)), 0)
        self.assertNotIn("match nothing", log)
        # the netlist differs only by what the hints change; the hint file did not break it
        self.assertTrue((third / "odin3" / f"{DEMO.stem}.blif").is_file())


class Hook(Case):
    """The abc hook: what is written, and that ABC uses the times."""

    def run_and11(self, hint_text: str | None, via: Path | None = None) -> tuple[str, str]:
        src = self.tmp / "and11.v"
        src.write_text(AND11)
        pre = ""
        if hint_text is not None:
            (self.tmp / "h.tsv").write_text(hint_text)
            pre = "read_hints h.tsv; "
        abc = "abc --lut 6" + (f" --via-blif {via}" if via is not None else "")
        proc = self.odin3(f"read_verilog and11.v; {pre}proc; flatten; lower; {abc}; "
                          f"write_blif --vtr out.blif")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return (self.tmp / "out.blif").read_text(), proc.stderr

    @staticmethod
    def last_lut_inputs(blif: str) -> list[str]:
        rows = [ln.split() for ln in blif.splitlines() if ln.startswith(".names")]
        return next(r[1:-1] for r in rows if r[-1] == "y")

    @needs_odin3
    def test_a_late_input_moves_toward_the_output(self) -> None:
        plain, _ = self.run_and11(None)
        self.assertNotIn("c", self.last_lut_inputs(plain))  # c is in the first of the two LUTs
        late, err = self.run_and11(HEAD + "net\ttop/c\t3000\t-\n")
        self.assertIn("c", self.last_lut_inputs(late))  # ABC used the arrival time of c
        self.assertIn("1 of 1 net hints match a net", err)
        self.assertIn("1 arrival times set", err)

    @needs_odin3
    def test_a_huge_time_is_clamped_not_undefined(self) -> None:
        _, err = self.run_and11(HEAD + "net\ttop/c\t1e300\t-\n")
        self.assertNotIn("runtime error", err)
        self.assertIn("1 arrival times set", err)

    @needs_odin3
    def test_no_matching_hint_leaves_the_netlist_alone(self) -> None:
        plain, _ = self.run_and11(None)
        same, err = self.run_and11(HEAD + "cell\t4294967295\t0\t1\t1\t1\nnet\ttop/nosuch\t900\t-\n")
        self.assertEqual(plain, same)
        self.assertIn("0 of 1 cell hints match a gate", err)
        self.assertIn("match nothing in the design", err)  # one WARNING with the counts

    @needs_odin3
    def test_the_command_file_and_timing_file(self) -> None:
        via = self.tmp / "via"
        self.run_and11(HEAD + "net\ttop/c\t3000\t-\n", via)
        cmds = (via / "abc0.cmd").read_text().splitlines()
        timing = cmds.index(next(c for c in cmds if c.startswith("o3_timing \"")))
        self.assertEqual(cmds[timing + 1].split()[0], "if")  # just before the mapper
        self.assertEqual(cmds[-2], "o3_timing -c")
        text = (via / "abc0.timing").read_text().splitlines()
        self.assertEqual(text[0], "default_required 2.000000")  # pass-1 depth
        self.assertIn("pi o3ci2 6.000000", text)  # 3000 ps at 0.5 ns per level
        plain = self.tmp / "via2"
        self.run_and11(None, plain)
        self.assertFalse(any(c.startswith("o3_timing") for c in (plain / "abc0.cmd").read_text()
                             .splitlines()))
        self.assertFalse((plain / "abc0.timing").exists())

    @needs_odin3
    def test_hints_are_refused_for_a_custom_script(self) -> None:
        script = self.tmp / "s.abc"
        script.write_text("strash\nif -K {K}\n")
        (self.tmp / "h.tsv").write_text(HEAD + "net\ttop/c\t3000\t-\n")
        src = self.tmp / "and11.v"
        src.write_text(AND11)
        proc = self.odin3(f"read_verilog and11.v; read_hints h.tsv; proc; flatten; lower; "
                          f"abc --lut 6 --script {script}; write_blif --vtr out.blif")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("cannot use them", proc.stderr)

    @needs_odin3
    def test_read_hints_errors_are_reported(self) -> None:
        (self.tmp / "bad.tsv").write_text("odin3-timing-hints\t1\nwhat\t1\n")
        proc = self.odin3("read_hints bad.tsv")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("bad.tsv:2: unknown record kind: what", proc.stderr)

    @needs_odin3
    def test_project_key_timing_hints(self) -> None:
        (self.tmp / "h.tsv").write_text(HEAD + "net\ttop/c\t3000\t-\n")
        (self.tmp / "and11.v").write_text(AND11)
        (self.tmp / "p.o3proj").write_text("timing_hints h.tsv\n")
        proc = self.odin3("read_project p.o3proj; read_verilog and11.v; proc; flatten; lower; "
                          "abc --lut 6; write_blif --vtr out.blif")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("1 of 1 net hints match a net", proc.stderr)
        dump = self.odin3("read_project p.o3proj; dump_project")
        self.assertIn("timing_hints", dump.stdout + dump.stderr)
        (self.tmp / "p2.o3proj").write_text("timing_hints h.tsv\ntiming_hints h.tsv\n")
        (self.tmp / "p3.o3proj").write_text("timing_hints bad.tsv\n")
        (self.tmp / "bad.tsv").write_text("not a hint file\n")
        gone = self.odin3("read_project p3.o3proj; read_verilog and11.v; proc; flatten; lower; "
                          "abc --lut 6")
        self.assertNotEqual(gone.returncode, 0)
        self.assertIn("bad.tsv:1: not a timing hint file", gone.stderr)
        self.assertIn("timing_hints file", gone.stderr)
        twice = self.odin3("read_project p2.o3proj")
        self.assertNotEqual(twice.returncode, 0)
        self.assertIn("timing_hints given twice", twice.stderr)


class Required(Case):
    """The required times the abc hook writes: depth + slack in levels - tighten_levels."""

    SCRIPT = ("read_verilog --nolatches --vtr-compat --prov-ast refs hint_demo.v; proc; flatten; "
              "lower; {pre}abc --lut 6 --prov-cut on{via}; write_blif --vtr --lut-k 6 "
              "--name-style hash --prov-map out.blif")

    def run_demo(self, pre: str = "", via: str = "") -> subprocess.CompletedProcess[str]:
        (self.tmp / "hint_demo.v").write_text(DEMO.read_text())
        return self.odin3(self.SCRIPT.format(pre=pre, via=via), env={"ODIN3_PROV_UID": "1"},
                         top="hint_demo")

    @needs_odin3
    def test_required_time_formula(self) -> None:
        first = self.run_demo()
        self.assertEqual(first.returncode, 0, first.stderr)
        blif = self.tmp / "out.blif"
        atoms = luts_of(blif)
        csv = self.tmp / "net.csv"
        fake_net_timing(csv, atoms, {a: 10.0 * i for i, a in enumerate(atoms)})
        hints = th.build(blif, blif.with_name("out.blif.names"), [csv], [1.0])[2]
        self.assertGreater(len(hints), 0)
        # every cell by hand: on the worst path (slack 0), pass-1 depth 3, 0.5 ns/level
        def table(slack: int) -> str:
            head = (HEAD.replace("cpd_ps\t1000", "cpd_ps\t1500").replace("depth\t2", "depth\t3")
                    + "meta\ttighten_levels\t1\n")
            return head + "".join(f"cell\t{h.uid}\t{slack}\t1\t3\t1\n" for h in hints)

        (self.tmp / "h.tsv").write_text(table(0))
        via = self.tmp / "via"
        second = self.run_demo("read_hints h.tsv; ", f" --via-blif {via}")
        self.assertEqual(second.returncode, 0, second.stderr)
        lines = (via / "abc0.timing").read_text().splitlines()
        self.assertEqual(lines[0], "default_required 3.000000")
        values = sorted(float(ln.split()[2]) for ln in lines[1:])
        self.assertTrue(values, "no required time written for the hinted cell")
        self.assertEqual(set(values), {2.0})  # 3 + 0 - 1: asked to be one level shallower
        self.assertIn(f"{len(hints)} of {len(hints)} cell hints match a gate", second.stderr)
        # more slack: 1000 ps over the worst at 0.5 ns per level is 2 levels of room
        (self.tmp / "h.tsv").write_text(table(1000))
        third = self.run_demo("read_hints h.tsv; ", f" --via-blif {self.tmp / 'via3'}")
        self.assertEqual(third.returncode, 0, third.stderr)
        more = (self.tmp / "via3" / "abc0.timing").read_text().splitlines()
        self.assertEqual({float(ln.split()[2]) for ln in more[1:]}, {4.0})  # 3 + 2 - 1


class FlowOptions(Case):
    def test_usage_errors(self) -> None:
        keep = {k: v for k, v in os.environ.items() if k != "VTR_ROOT"}
        with mock.patch.dict(os.environ, keep, clear=True):
            base = [str(DEMO), "--top", DEMO.stem, "--odin3", ODIN3 or "/bin/true"]
            _, _, err = run_main(flow.main, [*base, "--hints", str(self.tmp / "no.tsv")])
            self.assertIn("no such hint file", err)
            (self.tmp / "h.tsv").write_text(HEAD)
            _, _, err = run_main(flow.main, [*base, "--no-vpr", "--two-pass"])
            self.assertIn("--two-pass needs VPR", err)
            _, _, err = run_main(flow.main, [*base, "--target", "lpm", "--hints",
                                             str(self.tmp / "h.tsv")])
            self.assertIn("apply to --target vtr only", err)
            code, _, err = run_main(flow.main, [*base, "--hint-seeds", "0"])
            self.assertNotEqual(code, 0)
            self.assertIn("--hint-seeds needs a positive number", err)


class SeedRuns(Case):
    def test_extra_seeds_ask_for_the_net_timing_report(self) -> None:
        out = self.tmp / "tp"
        run_dir = out / "pass1" / "vpr" / "run"
        run_dir.mkdir(parents=True)
        (run_dir / two_pass.NET_TIMING).write_text("x\n")
        args = flow.FlowArgs([], "top", "vtr", None, "", True, False, Path("odin3"), out,
                             hint_seeds=3)
        seen: list[tuple[int, bool]] = []

        def run_flow(a: flow.FlowArgs) -> flow.FlowResult:
            return flow.FlowResult(True, summary={"crit_path_ns": 5.0}, vpr_run_dir=run_dir)

        def seed_run(a: flow.FlowArgs, blif: Path, job: Path) -> tuple[Path, float]:
            seen.append((a.vpr_seed, a.net_timing))
            job.mkdir(parents=True)
            (job / two_pass.NET_TIMING).write_text("x\n")
            return job, 6.0

        stop = th.HintError("stop here")
        with mock.patch.object(th, "build", side_effect=stop):
            two_pass.run(args, "top", run_flow, seed_run)
        self.assertEqual(seen, [(2, True), (3, True)])


@unittest.skipUnless(ODIN3 and VTR_ROOT, "ODIN3_CLI and VTR_ROOT needed (VPR runs)")
class TwoPass(Case):
    def test_two_pass_on_one_micro(self) -> None:
        out = self.tmp / "tp"
        env = dict(os.environ)
        with mock.patch.dict(os.environ, env, clear=True):
            code, _, err = run_main(flow.main, [str(DEMO), "--top", DEMO.stem, "--two-pass",
                                                "--odin3", ODIN3, "--out", str(out)])
        self.assertEqual(code, 0, err)
        hints = (out / "hints.tsv").read_text()
        self.assertTrue(hints.startswith("odin3-timing-hints\t1\n"))
        self.assertNotIn("/home" + "/", hints)  # no host path in a hint file
        self.assertTrue((out / "two-pass.json").is_file())
        log = (out / "pass2" / "odin3" / "odin3.log").read_text()
        self.assertRegex(log, r"abc: timing hints .*: (\d+) of \1 cell hints match a gate")
        self.assertTrue((out / "pass1" / "odin3" / f"{DEMO.stem}.blif").is_file())


if __name__ == "__main__":
    unittest.main()

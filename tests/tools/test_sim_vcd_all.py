"""``--vcd-all`` of ``sim`` and ``sim_tb``: the VCD holds one variable per net of the current
netlist view, named as ``write_blif --vtr`` names it. After ``abc --lut 6`` every output of the
written BLIF is a variable of the same name (also with ``--name-style hash``), each ``.names``
output equals its cover over the dumped inputs at every time stamp, and each ``re`` latch output
after the rising edge equals its input before it. Needs ``ODIN3_CLI``. Work directories are
removed when each test ends."""

from __future__ import annotations

import os
import re
import subprocess
import unittest
from pathlib import Path

from . import helpers

ODIN3 = os.environ.get("ODIN3_CLI", "")
ROOT = Path(__file__).resolve().parents[2]
TRAFFIC = ROOT / "tests" / "sim" / "traffic.v"
COUNTER = ROOT / "tests" / "tb" / "micro" / "counter.v"
COUNTER_TB = ROOT / "tests" / "tb" / "micro" / "counter_tb.v"
GOLDEN = ROOT / "tests" / "sim" / "vcd_all_counter.golden.vcd"
CONSTANTS = {"gnd": "0", "vcc": "1"}
PERIOD = 10
Cover = list[tuple[str, str]]


class Blif:
    """The ``.names`` covers and ``.latch`` lines of a one-model VTR BLIF."""

    def __init__(self, path: Path) -> None:
        self.names: list[tuple[list[str], str, Cover]] = []
        self.latches: list[tuple[str, str, str]] = []
        current: tuple[list[str], str, Cover] | None = None
        text = path.read_text().replace("\\\n", " ")
        for line in text.splitlines():
            word = line.split()
            if not word or word[0].startswith("#"):
                continue
            if word[0] == ".names":
                current = (word[1:-1], word[-1], [])
                self.names.append(current)
            elif word[0] == ".latch":
                current = None
                self.latches.append((word[1], word[2], word[4]))
            elif word[0].startswith("."):
                current = None
            elif current is not None:
                plane, out = (word[0], word[1]) if len(word) == 2 else ("", word[0])
                current[2].append((plane, out))

    def outputs(self) -> set[str]:
        """The nets a ``.names`` or ``.latch`` drives."""
        return {out for _, out, _ in self.names} | {out for _, out, _ in self.latches}


def evaluate(cover: Cover, values: list[str]) -> str:
    """A ``.names`` cover (rows of output 1 or only rows of output 0) over 0/1 input values."""
    hit = any(all(p in ("-", v) for p, v in zip(plane, values, strict=True)) for plane, _ in cover)
    on_set = bool(cover) and cover[0][1] == "1"
    return "1" if hit == on_set else "0"


class Vcd:
    """The variables by scope path, and the value of every code at every stamp."""

    def __init__(self, path: Path) -> None:
        self.scopes: dict[tuple[str, ...], dict[str, str]] = {}
        self.stamps: list[int] = []
        self.changes: dict[int, dict[str, str]] = {}
        stack: list[str] = []
        now = -1
        for line in path.read_text().splitlines():
            word = line.split()
            if not word:
                continue
            if word[0] == "$scope":
                stack.append(word[2])
            elif word[0] == "$upscope":
                stack.pop()
            elif word[0] == "$var":
                self.scopes.setdefault(tuple(stack), {})[word[4]] = word[3]
            elif word[0].startswith("#"):
                now = int(word[0][1:])
                self.stamps.append(now)
                self.changes[now] = {}
            elif now >= 0 and len(word) == 1 and word[0][0] in "01xz":
                self.changes[now][word[0][1:]] = word[0][0]

    def states(self) -> list[tuple[int, dict[str, str]]]:
        """(stamp, all values) for every stamp, in time order."""
        state: dict[str, str] = {}
        out = []
        for stamp in self.stamps:
            state.update(self.changes[stamp])
            out.append((stamp, dict(state)))
        return out


def run_odin3(script: str, cwd: Path, *, expect: int = 0) -> str:
    done = subprocess.run([ODIN3, "-p", script], capture_output=True, text=True, cwd=cwd,
                          check=False, timeout=300)
    if done.returncode != expect:
        raise AssertionError(f"odin3 exit {done.returncode}: {(done.stdout + done.stderr)[-400:]}")
    return done.stdout + done.stderr


@unittest.skipUnless(ODIN3 and Path(ODIN3).exists(), "odin3 (ODIN3_CLI) is needed")
class VcdAllTest(unittest.TestCase):
    lut = "proc; opt; lower; abc --lut 6"

    def check_netlist(self, blif: Blif, scope: dict[str, str], vcd: Vcd) -> None:
        """Names and values of the BLIF's nets against the VCD scope that holds them."""
        missing = {n for n in blif.outputs() if n not in scope and n not in CONSTANTS}
        self.assertEqual(missing, set(), "BLIF nets missing from the VCD")
        for stamp, state in vcd.states():
            for ins, out, cover in blif.names:
                if ins and out in scope and all(i in scope or i in CONSTANTS for i in ins):
                    vals = [CONSTANTS.get(i) or state[scope[i]] for i in ins]
                    self.assertEqual(state[scope[out]], evaluate(cover, vals),
                                     f"{out} at {stamp}")

    def test_sim_every_lut_output_is_a_variable(self) -> None:
        work = helpers.scratch(self, "test-vcd-all-")
        run_odin3(f"read_verilog {TRAFFIC}; {self.lut}; write_blif --vtr --lut-k 6 n.blif; "
                  "sim --vcd a.vcd --vcd-all 24 8; sim --vcd p.vcd 24 8", work)
        blif, vcd, plain = Blif(work / "n.blif"), Vcd(work / "a.vcd"), Vcd(work / "p.vcd")
        scope = vcd.scopes[("traffic",)]
        self.check_netlist(blif, scope, vcd)
        self.assertGreater(len(scope), len(plain.scopes[("traffic",)]))
        self.assertTrue(blif.names and blif.latches)

    def test_sim_latch_output_follows_input_at_the_edge(self) -> None:
        work = helpers.scratch(self, "test-vcd-all-")
        run_odin3(f"read_verilog {TRAFFIC}; {self.lut}; write_blif --vtr --lut-k 6 n.blif; "
                  "sim --vcd a.vcd --vcd-all 24 8", work)
        blif, vcd = Blif(work / "n.blif"), Vcd(work / "a.vcd")
        scope = vcd.scopes[("traffic",)]
        states = vcd.states()

        def at(when: int) -> dict[str, str]:
            return [st for stamp, st in states if stamp <= when][-1]

        for cyc in range(1, 24):
            before, after = at(PERIOD * cyc + 2), at(PERIOD * cyc + 5)
            for data, out, _ in blif.latches:
                self.assertEqual(after[scope[out]], before[scope[data]], f"{out}, cycle {cyc}")

    def test_hash_names_are_the_written_ones(self) -> None:
        work = helpers.scratch(self, "test-vcd-all-")
        run_odin3(f"read_verilog {TRAFFIC}; {self.lut}; "
                  "write_blif --vtr --lut-k 6 --name-style hash h.blif; "
                  "sim --vcd a.vcd --vcd-all --vcd-names hash 12 3", work)
        blif, vcd = Blif(work / "h.blif"), Vcd(work / "a.vcd")
        self.assertTrue(any(re.fullmatch(r"h[0-9a-z]{10}", n) for n in blif.outputs()))
        self.check_netlist(blif, vcd.scopes[("traffic",)], vcd)

    def test_sim_rtl_view_dumps_nets_with_port_bit_names(self) -> None:
        work = helpers.scratch(self, "test-vcd-all-")
        run_odin3(f"read_verilog {TRAFFIC}; proc; opt; lower; sim --vcd a.vcd --vcd-all 8 1", work)
        names = Vcd(work / "a.vcd").scopes[("traffic",)]
        for port in ("clk", "light_ns[0]", "light_ns[1]", "light_ew[1]"):
            self.assertIn(port, names)

    def test_sim_tb_dumps_the_dut_nets_in_a_nested_scope(self) -> None:
        work = helpers.scratch(self, "test-vcd-all-")
        run_odin3(f"read_verilog {COUNTER}; {self.lut}; write_blif --vtr --lut-k 6 n.blif; "
                  f"sim_tb --top counter_tb --vcd a.vcd --vcd-all {COUNTER_TB}", work)
        blif, vcd = Blif(work / "n.blif"), Vcd(work / "a.vcd")
        self.assertIn("q", vcd.scopes[("counter_tb",)])
        self.assertNotIn("$n11", vcd.scopes[("counter_tb", "dut")])
        self.check_netlist(blif, vcd.scopes[("counter_tb", "dut", "counter")], vcd)

    def test_sim_tb_golden(self) -> None:
        work = helpers.scratch(self, "test-vcd-all-")
        run_odin3(f"read_verilog {COUNTER}; {self.lut}; "
                  f"sim_tb --top counter_tb --vcd a.vcd --vcd-all {COUNTER_TB}", work)
        got = (work / "a.vcd").read_text()
        if os.environ.get("ODIN3_UPDATE_GOLDEN"):
            GOLDEN.write_text(got)
        self.assertEqual(got, GOLDEN.read_text())

    def test_sim_tb_without_vcd_all_is_unchanged(self) -> None:
        work = helpers.scratch(self, "test-vcd-all-")
        run_odin3(f"read_verilog {COUNTER}; {self.lut}; "
                  f"sim_tb --top counter_tb --vcd a.vcd {COUNTER_TB}", work)
        self.assertEqual(set(Vcd(work / "a.vcd").scopes), {("counter_tb",), ("counter_tb", "dut")})

    def test_options_need_each_other(self) -> None:
        work = helpers.scratch(self, "test-vcd-all-")
        base = f"read_verilog {TRAFFIC}; proc; opt; lower; "
        for script in ("sim --vcd-all 4 1", "sim --vcd a.vcd --vcd-names hash 4 1",
                       "sim --vcd a.vcd --vcd-all --vcd-names odd 4 1",
                       f"sim_tb --vcd-all {COUNTER_TB}",
                       f"sim_tb --vcd a.vcd --vcd-names hash {COUNTER_TB}"):
            out = run_odin3(base + script, work, expect=1)
            self.assertRegex(out, r"vcd-all|vcd-names|expects", script)


if __name__ == "__main__":
    unittest.main()

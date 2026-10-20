"""Tests for tools/fsm-check/yosys_log.py and fsm_probe.py (4.8 plan Task 1).

The log excerpts are built in the format Yosys 0.55 prints (fsm_detect, fsm_extract, fsm_recode
and FsmData::log_info), with the state and transition counts of two real runs: ch_intrinsics
``cur_state`` (15 states, one-hot, 15 bits) and mcml ``LogCalc.r_indexFirstOne`` (32 states,
binary, 6 to 5 bits).
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import fsm_probe
import yosys_log

from . import helpers


def _binary(value: int, bits: int) -> str:
    return "".join("1" if (value >> k) & 1 else "0" for k in range(bits))  # bit 0 first


def _info(cell: str, wire: str, counts: tuple[int, int, int, int, int], module: str) -> str:
    ins, outs, bits, states, trans = counts
    out = [
        f"FSM `{cell}' from module `{module}':",
        "-" * 37,
        f"  Information on FSM {cell} (\\{wire}):",
        "",
        f"  Number of input signals:  {ins:3d}",
        f"  Number of output signals: {outs:3d}",
        f"  Number of state bits:     {bits:3d}",
        "",
        "  State encoding:",
    ]
    for s in range(states):
        out.append(f"    {s}: {bits:5d}'{'0' * bits}" + ("  <RESET STATE>" if s == 0 else ""))
    out += ["", "  Transition Table (state_in, ctrl_in, state_out, ctrl_out):"]
    out += [f"    {t:3d}:     0 1'0   ->     0 1'0" for t in range(trans)]
    out += ["", "-" * 37]
    return "\n".join(out)


CH_INTRINSICS = "\n".join(
    [
        "Found FSM state register memset.cur_state.",
        "Extracting FSM `\\cur_state' from module `\\memset'.",
        "Recoding FSM `$fsm$\\cur_state$266' from module `\\memset' using `auto' encoding:",
        "  mapping auto encoding to `one-hot` for this FSM.",
        *[f"  {_binary(s, 4)} -> {'-' * (14 - s)}1{'-' * s}" for s in range(15)],
        _info("$fsm$\\cur_state$266", "cur_state", (5, 16, 15, 15, 38), "memset"),
    ]
)

MCML = "\n".join(
    [
        "Found FSM state register LogCalc.r_indexFirstOne.",
        "Extracting FSM `\\r_indexFirstOne' from module `\\LogCalc'.",
        "Recoding FSM `$fsm$\\r_indexFirstOne$18550' from module `\\LogCalc' using `auto' encoding:",
        "  mapping auto encoding to `binary` for this FSM.",
        *[f"  {_binary(s, 6)} -> {_binary(s, 5)}" for s in range(32)],
        _info("$fsm$\\r_indexFirstOne$18550", "r_indexFirstOne", (33, 7, 5, 32, 70), "LogCalc"),
    ]
)

NOT_MARKING = """\
Not marking raysend.rgWE as FSM state register:
    Register is connected to module port.
    Users of register don't seem to benefit from recoding.
Found FSM state register raysend.state.
Extracting FSM `\\state' from module `\\raysend'.
  found state code: 4'0000
  fsm extraction failed: at least two states are required.
"""


class ParseTest(unittest.TestCase):
    def test_ch_intrinsics_one_hot(self) -> None:
        facts = yosys_log.parse_log(CH_INTRINSICS)["memset"]
        self.assertEqual(facts.marked, ["cur_state"])
        (fsm,) = facts.fsms
        self.assertEqual((fsm.states, fsm.encoding), (15, "one-hot"))
        self.assertEqual((fsm.bits_before, fsm.bits_after), (4, 15))
        self.assertEqual((fsm.inputs, fsm.outputs, fsm.transitions), (5, 16, 38))
        self.assertEqual(fsm.reset_index, 0)
        self.assertEqual(fsm.wire, "cur_state")

    def test_mcml_binary_six_to_five(self) -> None:
        (fsm,) = yosys_log.parse_log(MCML)["LogCalc"].fsms
        self.assertEqual((fsm.states, fsm.encoding), (32, "binary"))
        self.assertEqual((fsm.bits_before, fsm.bits_after), (6, 5))
        self.assertEqual(fsm.wire, "r_indexFirstOne")

    def test_not_marking_two_reasons_and_failure(self) -> None:
        facts = yosys_log.parse_log(NOT_MARKING)["raysend"]
        self.assertEqual(
            facts.rejected["rgWE"],
            [
                "Register is connected to module port",
                "Users of register don't seem to benefit from recoding",
            ],
        )
        self.assertEqual(facts.marked, ["state"])
        self.assertEqual(facts.failures, [("state", "at least two states are required")])

    def test_packed_binary_keeps_codes(self) -> None:
        text = (
            "Recoding FSM `$fsm$\\st$14' from module `\\t1' using `binary' encoding:\n"
            "  existing encoding is already a packed binary encoding.\n"
            + _info("$fsm$\\st$14", "st", (1, 1, 2, 3, 4), "t1")
        )
        (fsm,) = yosys_log.parse_log(text)["t1"].fsms
        self.assertEqual((fsm.encoding, fsm.bits_before, fsm.bits_after), ("binary", 2, 2))

    def test_none_encoding_is_reported(self) -> None:
        text = (
            "Recoding FSM `$fsm$\\st$3' from module `\\t1' using `none' encoding:\n"
            "  nothing to do for encoding `none'.\n"
        )
        self.assertEqual(yosys_log.parse_log(text), {})


class ScriptTest(unittest.TestCase):
    def test_vtr_script_has_primitives_and_setattr(self) -> None:
        script = fsm_probe.probe_script(["a.v"], vtr=True)
        self.assertIn("+/parmys/vtr_primitives.v", script)
        self.assertIn("setattr -mod -set keep_hierarchy 1 single_port_ram", script)
        self.assertTrue(script.rstrip().endswith("proc -norom; fsm"))
        self.assertIn("opt_expr; opt_clean; opt -nodffe -nosdff; proc -norom", script)

    def test_micro_script_has_no_setattr(self) -> None:
        script = fsm_probe.probe_script(["a.v"], vtr=False, top="t")
        self.assertNotIn("setattr", script)
        self.assertIn("hierarchy -check -top t", script)

    def test_table_and_json(self) -> None:
        facts = {"memset": yosys_log.parse_log(CH_INTRINSICS)["memset"]}
        rows = fsm_probe.rows_of("ch", facts)
        self.assertEqual(rows[0]["states"], 15)
        table = fsm_probe.format_table(rows)
        self.assertIn("one-hot", table)
        json.loads(fsm_probe.rows_json(rows))

    def test_main_reports_missing_yosys(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "a.v"
            src.write_text("module a; endmodule\n")
            code, _, err = helpers.run_main(
                fsm_probe.main, [str(src), "--yosys", str(Path(tmp) / "nope")]
            )
        self.assertEqual(code, 2)
        self.assertIn("yosys", err)


class CommittedFactsTest(unittest.TestCase):
    """tests/fsm/yosys_facts.json reproduces the numbers of spec section 12."""

    facts: dict[str, dict[str, list[dict[str, object]]]]

    @classmethod
    def setUpClass(cls) -> None:
        path = helpers.REPO_ROOT / "tests/fsm/yosys_facts.json"
        cls.facts = json.loads(path.read_text())

    def vtr_designs(self) -> dict[str, dict[str, list[dict[str, object]]]]:
        return {k: v for k, v in self.facts.items() if not k.startswith(("micro/", "fsm/"))}

    def test_24_user_fsms_in_9_designs(self) -> None:
        designs = self.vtr_designs()
        user = {
            k: [f for f in v["fsms"] if "memwr" not in str(f["wire"])] for k, v in designs.items()
        }
        self.assertEqual(len(designs), 9)
        self.assertEqual(sum(len(v) for v in user.values()), 24)
        self.assertTrue(all(user.values()))

    def test_mcml_is_the_binary_case(self) -> None:
        fsms = [f for f in self.facts["mcml"]["fsms"] if f["wire"] == "r_indexFirstOne"]
        self.assertEqual(len(fsms), 1)
        self.assertEqual((fsms[0]["states"], fsms[0]["encoding"]), (32, "binary"))
        self.assertEqual((fsms[0]["bits_before"], fsms[0]["bits_after"]), (6, 5))
        others = [f for v in self.vtr_designs().values() for f in v["fsms"] if f is not fsms[0]]
        self.assertTrue(all(f["encoding"] == "one-hot" for f in others))

    def test_25_micros_have_a_detected_register(self) -> None:
        micros = {k: v for k, v in self.facts.items() if k.startswith("micro/")}
        self.assertEqual(len(micros), 25)
        self.assertTrue(all(v["marked"] for v in micros.values()))

    def test_ch_intrinsics_cur_state(self) -> None:
        (fsm,) = [f for f in self.facts["ch_intrinsics"]["fsms"] if f["wire"] == "cur_state"]
        self.assertEqual((fsm["states"], fsm["bits_after"], fsm["encoding"]), (15, 15, "one-hot"))


if __name__ == "__main__":
    unittest.main()

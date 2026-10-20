"""Tests for tools/fsm-check/fsm_diff.py and odin_log.py (4.8 plan Task 10, spec T-3).

The logs are real: the Yosys 0.55 text and Odin III's text for the 3-state machine of spec section
12 (tests/fsm/moore_arst.v), cut to the lines the parsers read.
"""

from __future__ import annotations

import tempfile
import unittest
from collections import Counter
from pathlib import Path

import fsm_diff
import odin_log
import yosys_log

YOSYS = """\
Found FSM state register moore_arst.st.
Recoding FSM `$fsm$\\st$13' from module `\\moore_arst' using `auto' encoding:
  mapping auto encoding to `one-hot` for this FSM.
  00 -> --1
  10 -> -1-
  01 -> 1--
FSM `$fsm$\\st$13' from module `moore_arst':
-------------------------------------

  Information on FSM $fsm$\\st$13 (\\st):

  Number of input signals:    1
  Number of output signals:   1
  Number of state bits:       3

  Input signals:
    0: \\go

  Output signals:
    0: \\o

  State encoding:
    0:      3'--1  <RESET STATE>
    1:      3'-1-
    2:      3'1--

  Transition Table (state_in, ctrl_in, state_out, ctrl_out):
      0:     0 1'0   ->     0 1'0
      1:     0 1'1   ->     2 1'0
      2:     1 1'-   ->     0 1'1
      3:     2 1'-   ->     1 1'0

-------------------------------------
"""

ODIN = """\
odin3: info: fsm: found state register 'moore_arst.st'
odin3: info: fsm: extracting 'moore_arst.st': 3 states, 1 control inputs, 6 control outputs
odin3: info: fsm: recoding 'moore_arst.st' using 'auto' encoding
odin3: info: fsm: 'moore_arst.st': mapping auto encoding to 'one-hot'
odin3: info: fsm: recoding 'moore_arst.st' with one-hot encoding (3 states, 3 bits, was 2)
odin3: info: fsm: information on FSM 'moore_arst.st':
odin3: info: fsm:   number of input signals:    1
odin3: info: fsm:   number of output signals:   1
odin3: info: fsm:   number of state bits:       3
odin3: info: fsm:   input signals:
odin3: info: fsm:     0: go
odin3: info: fsm:   output signals:
odin3: info: fsm:     0: o
odin3: info: fsm:   state encoding:
odin3: info: fsm:     0:      3'--1  <RESET STATE>
odin3: info: fsm:     1:      3'-1-
odin3: info: fsm:     2:      3'1--
odin3: info: fsm:   transition table (state_in, ctrl_in, state_out, ctrl_out):
odin3: info: fsm:       0:     0 1'0   ->     0 1'0
odin3: info: fsm:       1:     0 1'1   ->     2 1'0
odin3: info: fsm:       2:     1 1'-   ->     0 1'1
odin3: info: fsm:       3:     2 1'-   ->     1 1'0
odin3: info: pass fsm: 0.7 ms
"""


class ParseTest(unittest.TestCase):
    def test_both_logs_give_the_same_record(self) -> None:
        yos = yosys_log.parse_log(YOSYS)["moore_arst"].fsms[0]
        odi = odin_log.parse_log(ODIN)["moore_arst"].fsms[0]
        self.assertEqual(fsm_diff.shape(yos), fsm_diff.shape(odi))
        self.assertEqual(
            (odi.states, odi.bits_before, odi.bits_after, odi.encoding), (3, 2, 3, "one-hot")
        )
        self.assertEqual(odi.reset_index, 0)
        self.assertEqual(odi.codes, ("--1", "-1-", "1--"))

    def test_rejected_and_failed_lines(self) -> None:
        text = (
            "odin3: info: fsm: not marking 'm.a': connected to a module port\n"
            "odin3: info: fsm: not marking 'm.a': users do not benefit from recoding\n"
            "odin3: info: fsm: extraction of 'm.b' failed: at least two states are required\n"
        )
        facts = odin_log.parse_log(text)["m"]
        self.assertEqual(
            facts.rejected["a"],
            [
                "Register is connected to module port",
                "Users of register don't seem to benefit from recoding",
            ],
        )
        self.assertEqual(facts.failures, [("b", "at least two states are required")])

    def test_a_dotted_module_name_splits_at_the_last_dot(self) -> None:
        facts = odin_log.parse_log("odin3: info: fsm: found state register 'a.b.st'\n")
        self.assertEqual(facts["a.b"].marked, ["st"])


class CompareTest(unittest.TestCase):
    def mods(self, odin_text: str) -> list[fsm_diff.Diff]:
        return fsm_diff.compare_facts(yosys_log.parse_log(YOSYS), odin_log.parse_log(odin_text))

    def test_equal_facts_pass(self) -> None:
        self.assertEqual(self.mods(ODIN), [])

    def test_a_different_state_count_fails(self) -> None:
        text = ODIN.replace("(3 states, 3 bits, was 2)", "(4 states, 3 bits, was 2)")
        text = text.replace(
            "fsm:     2:      3'1--\n",
            "fsm:     2:      3'1--\nodin3: info: fsm:     3:      3'1-1\n",
        )
        diffs = self.mods(text)
        self.assertEqual([d.stage for d in diffs], ["facts"])
        self.assertIn("states Yosys 3 Odin III 4", diffs[0].text)

    def test_a_different_encoding_fails(self) -> None:
        diffs = self.mods(ODIN.replace("one-hot encoding (", "binary encoding ("))
        self.assertEqual([d.stage for d in diffs], ["facts"])
        self.assertIn("encoding Yosys one-hot Odin III binary", diffs[0].text)

    def test_a_register_marked_on_one_side_only(self) -> None:
        diffs = self.mods(ODIN + "odin3: info: fsm: found state register 'moore_arst.extra'\n")
        self.assertEqual([(d.stage, "Odin III only" in d.text) for d in diffs], [("detect", True)])

    def test_a_missing_fsm_fails(self) -> None:
        diffs = fsm_diff.compare_facts(yosys_log.parse_log(YOSYS), {})
        self.assertEqual({d.stage for d in diffs}, {"detect", "facts"})

    def test_the_transition_shape_decides_not_the_order_of_signals(self) -> None:
        swapped = ODIN.replace("0 1'1   ->     2 1'0", "0 1'0   ->     2 1'0")
        self.assertEqual([d.stage for d in self.mods(swapped)], [])  # same shape: only bits move
        broken = ODIN.replace("1 1'-   ->     0 1'1", "1 1'-   ->     0 1'0")
        self.assertEqual([d.stage for d in self.mods(broken)], ["facts"])


class ExpectTest(unittest.TestCase):
    EXPECT = [("d1", "facts", "a reason"), ("d1", "map", "another"), ("d2", "native-detect", "x")]

    def test_a_listed_difference_is_absorbed(self) -> None:
        diffs = [fsm_diff.Diff("facts", "x"), fsm_diff.Diff("detect", "y")]
        left, used = fsm_diff.apply_expect("d1", diffs, self.EXPECT)
        self.assertEqual([d.stage for d in left], ["detect"])
        self.assertEqual(used, {("d1", "facts")})

    def test_a_stale_line_is_reported(self) -> None:
        stale = fsm_diff.stale_lines(self.EXPECT, {("d1", "facts")}, ["d1"])
        self.assertEqual(stale, ["d1 map"])

    def test_native_lines_and_other_designs_are_not_stale(self) -> None:
        self.assertEqual(fsm_diff.stale_lines(self.EXPECT, set(), ["d2"]), [])
        self.assertEqual(fsm_diff.stale_lines(self.EXPECT, set(), ["d3"]), [])

    def test_a_line_needs_a_reason(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "expect.txt"
            path.write_text("# c\nname stage\n")
            with self.assertRaises(fsm_diff.DiffError):
                fsm_diff.read_expect(path)

    def test_the_committed_list_parses(self) -> None:
        rows = fsm_diff.read_expect(fsm_diff.DEFAULT_EXPECT)
        self.assertTrue(rows)
        self.assertTrue(all(reason for _, _, reason in rows))


class HistogramTest(unittest.TestCase):
    TEXT = (
        "module \\m\n  cell $eq \\a\n  end\n  cell $eq \\b\n  end\n"
        "  cell $dff \\c\n  end\nend\n"
        "module \\n\n  cell $and \\d\n  end\nend\n"
    )

    def test_counts_per_module(self) -> None:
        hist = fsm_diff.histograms(self.TEXT)
        self.assertEqual(hist["m"], Counter({"$eq": 2, "$dff": 1}))
        self.assertEqual(hist["n"], Counter({"$and": 1}))

    def test_delta_and_compare(self) -> None:
        before = fsm_diff.histograms(self.TEXT)
        after = fsm_diff.histograms(self.TEXT.replace("$eq \\b", "$or \\b"))
        delta = fsm_diff.hist_delta(after, before)
        self.assertEqual(delta["m"], Counter({"$eq": -1, "$or": 1}))
        self.assertEqual(fsm_diff.compare_histograms(delta, delta), [])
        diffs = fsm_diff.compare_histograms(delta, fsm_diff.hist_delta(before, before))
        self.assertEqual([d.stage for d in diffs], ["map"])


if __name__ == "__main__":
    unittest.main()

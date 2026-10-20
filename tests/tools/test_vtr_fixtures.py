"""Tests for tools/blif-vtr-check/vtr_fixtures.py: the round trip of 3E §16.3 step 1, as the 3E-1
final review's erratum amends it (I1). A write that legalised no name must give W1 == W2 byte for
byte; a write that legalised names is compared under the VB-10 decode instead, since VB-10 escapes
the `%` of its own escapes again (`\\` -> `%5C` -> `%255C`).

The end-to-end cases run odin3 (``ODIN3_CLI``, set by CTest python_tools) on two committed
fixtures: ``shiftx.parmys`` (a Parmys k6 golden with a mid-name `\\`, one of 25) and ``two_clock``.
"""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

import vtr_fixtures

from . import helpers

FIXTURES = helpers.REPO_ROOT / "tests" / "golden" / "blif-vtr"
ODIN3 = os.environ.get("ODIN3_CLI")


class Vb10DecodeTest(unittest.TestCase):
    def test_one_write_inverted(self) -> None:
        # plan 3E point (o): upper-case hex is a byte, anything else after `%` the rule-3 prefix
        cases = {
            b"%fe": b"fe",
            b"%FE": b"\xfe",
            b"a%23b": b"a#b",
            b"%2Eend": b".end",
            b"%out:%5Cx": b"out:\\x",
            b"plain[3]": b"plain[3]",
        }
        for written, name in cases.items():
            with self.subTest(written=written):
                self.assertEqual(name, vtr_fixtures.vb10_decode(written))

    def test_fixed_point(self) -> None:
        # a name written twice decodes like one written once
        once, twice = b"$2%5Cbuffer[7:0][0]", b"$2%255Cbuffer[7:0][0]"
        self.assertEqual(b"$2\\buffer[7:0][0]", vtr_fixtures.vb10_decode(once))
        self.assertEqual(vtr_fixtures.vb10_decode(once), vtr_fixtures.vb10_decode(twice))
        self.assertEqual(b"NIL", vtr_fixtures.vb10_decode(b"%25NIL"))


W1 = b"# header\n\n.model m\n.inputs a%5Cb\n.outputs y\n.names a%5Cb \\\n  y\n1 1\n.end\n"
W2 = b"# header\n\n.model m\n.inputs a%255Cb\n.outputs y\n.names a%255Cb y\n1 1\n.end\n"


class RoundTripVerdictTest(unittest.TestCase):
    def test_renamed_compared_decoded(self) -> None:
        self.assertEqual((None, "decoded"), vtr_fixtures.round_trip_verdict(W1, W2, 2))

    def test_renamed_structural_difference_fails(self) -> None:
        why, mode = vtr_fixtures.round_trip_verdict(W1, W2.replace(b"1 1", b"0 1"), 2)
        self.assertEqual("decoded", mode)
        self.assertIsNotNone(why)

    def test_not_renamed_needs_bytes(self) -> None:
        self.assertEqual((None, "bytes"), vtr_fixtures.round_trip_verdict(W1, W1, 0))
        why, mode = vtr_fixtures.round_trip_verdict(W1, W2, 0)
        self.assertEqual("bytes", mode)
        self.assertIsNotNone(why)


class CaseArgsTest(unittest.TestCase):
    """A case's extra write_blif options come from <name>.args (3E-6: --name-style provenance)."""

    def test_args_file_is_read_and_owns_no_orphan(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vtr-args.") as tmp:
            fixtures = Path(tmp)
            (fixtures / "p.in.blif").write_text(".model p\n.end\n", encoding="utf-8")
            (fixtures / "q.in.blif").write_text(".model q\n.end\n", encoding="utf-8")
            (fixtures / "p.args").write_text("--name-style provenance\n", encoding="utf-8")
            cases = {c.name: c for c in vtr_fixtures.find_cases(fixtures)}
            self.assertEqual("--name-style provenance", cases["p"].args)
            self.assertEqual("", cases["q"].args)
            self.assertEqual([], vtr_fixtures.orphans(fixtures, list(cases.values())))
            (fixtures / "z.args").write_text("x", encoding="utf-8")
            self.assertEqual(["z.args"], vtr_fixtures.orphans(fixtures, list(cases.values())))

    def test_the_script_carries_the_args(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vtr-args.") as tmp:
            fake = Path(tmp) / "odin3"
            fake.write_text('#!/bin/sh\necho "$2" >&2\n', encoding="utf-8")
            fake.chmod(0o755)
            proc = vtr_fixtures.rewrite(
                fake, Path(tmp) / "a.blif", Path(tmp) / "o.blif", "--name-style provenance"
            )
            self.assertIn(b'write_blif --vtr --name-style provenance "', proc.stderr)
            plain = vtr_fixtures.rewrite(fake, Path(tmp) / "a.blif", Path(tmp) / "o.blif")
            self.assertIn(b'write_blif --vtr "', plain.stderr)


@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (run CTest python_tools)")
class FixtureRoundTripTest(unittest.TestCase):
    def round_trip(self, name: str, work: Path) -> tuple[int, bytes, bytes]:
        """W1 and W2 of fixture name in work, and the `renamed` count of W1's write."""
        odin3 = Path(str(ODIN3))
        case = vtr_fixtures.Case(name, FIXTURES / (name + vtr_fixtures.IN_SUFFIX))
        outcome = vtr_fixtures.run_case(odin3, case, work)
        self.assertEqual("", outcome.detail)
        w1 = work / (name + vtr_fixtures.OUT_SUFFIX)
        w2 = work / (name + ".w2.blif")
        proc = vtr_fixtures.rewrite(odin3, w1, w2)
        self.assertEqual(0, proc.returncode, proc.stderr.decode(errors="replace"))
        return outcome.renamed, w1.read_bytes(), w2.read_bytes()

    def test_shiftx_compared_decoded_not_bytes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vtr-rt.") as tmp:
            renamed, w1, w2 = self.round_trip("shiftx.parmys", Path(tmp))
            self.assertEqual(8, renamed)
            self.assertIn(b"$2%5Cbuffer[7:0][0]", w1)
            self.assertIn(b"$2%255Cbuffer[7:0][0]", w2)
            self.assertNotEqual(w1, w2)  # byte idempotence cannot hold: it is not applied
            self.assertEqual((None, "decoded"), vtr_fixtures.round_trip_verdict(w1, w2, renamed))
            log = io.StringIO()
            with contextlib.redirect_stdout(log):
                why = vtr_fixtures.round_trip(Path(str(ODIN3)), "shiftx.parmys", Path(tmp), renamed)
            self.assertIsNone(why)
            self.assertIn("note shiftx.parmys: round trip: 8 names legalised", log.getvalue())
            self.assertIn("(byte comparison not applied)", log.getvalue())

    def test_two_clock_bytes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vtr-rt.") as tmp:
            renamed, w1, w2 = self.round_trip("two_clock", Path(tmp))
            self.assertEqual(0, renamed)
            self.assertEqual(w1, w2)
            self.assertEqual((None, "bytes"), vtr_fixtures.round_trip_verdict(w1, w2, renamed))


CHECKER = helpers.REPO_ROOT / "tools" / "blif-vtr-check" / "blif-vtr-check"


class StrictExceptionTest(unittest.TestCase):
    """The checker step: --strict on every expected file, but STRICT_EXCEPTIONS without it, and
    an exception that --strict no longer needs fails."""

    def check(self, name: str, blif: Path) -> tuple[str | None, str]:
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            why = vtr_fixtures.check_expected(CHECKER, name, blif)
        return why, log.getvalue()

    def test_shiftx_needs_its_exception(self) -> None:
        blif = FIXTURES / "shiftx.parmys.vtr.blif"
        why, log = self.check("shiftx.parmys", blif)
        self.assertIsNone(why)
        self.assertIn("note shiftx.parmys: blif-vtr-check without --strict (C-S2", log)
        why, log = self.check("not_excepted", blif)
        self.assertEqual("blif-vtr-check --strict exited 1", why)
        self.assertIn("net 'unconn' feeds a .names/.latch pin", log)

    @unittest.skipUnless(ODIN3, "ODIN3_CLI not set (run CTest python_tools)")
    def test_writer_log_pairs_shiftx(self) -> None:
        # Task 11's oracle (spec §16.4): a W whose C-S2 lines pair with the writer's class-U
        # warning passes --strict --writer-log; the plain --strict run still fails.
        with tempfile.TemporaryDirectory(prefix="vtr-log.") as tmp:
            w = Path(tmp) / "w.blif"
            proc = vtr_fixtures.rewrite(Path(str(ODIN3)), FIXTURES / "shiftx.parmys.in.blif", w)
            self.assertEqual(0, proc.returncode)
            log = Path(tmp) / "w.log"
            log.write_bytes(proc.stderr)
            self.assertIn(b"net 'unconn' feeds 7 .names/.latch pins", proc.stderr)
            self.assertEqual(1, vtr_fixtures.check_file(CHECKER, w, True).returncode)
            paired = subprocess.run(
                [str(CHECKER), "--strict", "--writer-log", str(log), str(w)],
                capture_output=True,
                text=True,
                check=False,
                timeout=60,
            )
            self.assertEqual((0, ""), (paired.returncode, paired.stdout))

    def test_stale_exception_fails(self) -> None:
        why, _ = self.check("shiftx.parmys", FIXTURES / "two_clock.vtr.blif")
        self.assertEqual(
            "in STRICT_EXCEPTIONS but passes blif-vtr-check --strict (drop the exception)", why
        )


if __name__ == "__main__":
    unittest.main()

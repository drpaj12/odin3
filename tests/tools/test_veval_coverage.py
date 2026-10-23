"""Tests for tools/veval/veval-coverage.

The classification is exercised on the exact CLI log shapes (a located ``read_slang:`` error, a
pass error without a location, a sanitizer report and a signal). ``check_manifest`` runs a fake
odin3 that writes the BLIF the tool expects and then behaves per ``VVEVAL_FAKE``: ``good`` exits
0, ``refuse:TEXT`` errors, ``crash`` aborts. That proves a previously-ok design regressing to a
refusal or a crash fails the corpus check, and that a changed refusal message or class fails it.
"""

from __future__ import annotations

import contextlib
import io
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

import veval_coverage

FAKE_ODIN3 = """#!{python}
import os, shlex, sys

script = sys.argv[sys.argv.index("-p") + 1]
words = shlex.split(script)
out = words[words.index("--gates-as-names") + 1]
with open(out, "w") as f:
    f.write(".model top\\n.end\\n")
ref = os.path.basename(words[words.index("read_verilog") + 1].rstrip(";"))
mode = os.environ.get("VVEVAL_FAKE_" + ref, os.environ.get("VVEVAL_FAKE", "good"))
if mode == "good":
    sys.exit(0)
if mode == "crash":
    os.abort()
if mode.startswith("refuse:"):
    sys.stderr.write("odin3: error: read_slang: REF:3:4: " + mode[len("refuse:"):] + "\\n")
    sys.exit(1)
sys.exit(2)
"""


def located(message: str) -> str:
    return f"odin3: error: read_slang: /refs/Prob001_zero_ref.sv:4:9: {message}\n"


class ClassifyTest(unittest.TestCase):
    def test_ok(self) -> None:
        self.assertEqual(veval_coverage.classify(0, ""), veval_coverage.Verdict("ok"))

    def test_every_refusal_class(self) -> None:
        cases = {
            "'unique case' is not supported": "unique_or_priority",
            "'priority if' is not supported": "unique_or_priority",
            "an explicit 4-bit cast is not supported": "explicit_cast",
            "an intra-assignment timing control is not supported": "intra_assignment_timing",
            "a drive strength on net 'a' is not supported": "drive_strength",
            "a drive strength on a continuous assignment is not supported": "drive_strength",
            "ref port 'x' is not supported": "ref_port",
            "ref modport port 'y' is not supported": "ref_port",
            "member 'a' of a type with no bit layout": "union_member",
            "function and task calls are not supported yet": "other",
        }
        for message, expected in cases.items():
            verdict = veval_coverage.classify(1, located(message))
            self.assertEqual(verdict.kind, "refused", message)
            self.assertEqual(verdict.refusal, expected, message)
            self.assertEqual(verdict.message, message)

    def test_location_and_tag_are_stripped(self) -> None:
        error = "read_slang: /refs/Prob156_review2015_fancytimer_ref.sv:25:21: " \
                "an explicit 4-bit cast is not supported"
        self.assertEqual(
            veval_coverage.pinned_text(error), "an explicit 4-bit cast is not supported"
        )
        # A message without a location keeps everything but the tag.
        self.assertEqual(
            veval_coverage.pinned_text("read_slang: the sources define no module"),
            "the sources define no module",
        )

    def test_crash_markers_and_signals(self) -> None:
        self.assertEqual(
            veval_coverage.classify(-6, "").kind,
            "crash",
        )
        self.assertEqual(
            veval_coverage.classify(1, "==1==ERROR: AddressSanitizer: heap-buffer-overflow\n").kind,
            "crash",
        )
        self.assertEqual(
            veval_coverage.classify(1, "x.c:1:2: runtime error: signed integer overflow\n").kind,
            "crash",
        )

    def test_nonzero_without_diagnostic_is_a_crash(self) -> None:
        self.assertEqual(veval_coverage.classify(3, "").kind, "crash")

    def test_pass_summary_does_not_hide_the_diagnostic(self) -> None:
        stderr = (
            "odin3: info: pass read_techlib: 0.3 ms\n"
            + located("an explicit 4-bit cast is not supported")
            + "odin3: error: pass read_verilog: failed: ODIN3_ERR_PARSE\n"
            "odin3: error: -p: command 2: pass 'read_verilog' failed: ODIN3_ERR_PARSE\n"
        )
        verdict = veval_coverage.classify(1, stderr)
        self.assertEqual(verdict.message, "an explicit 4-bit cast is not supported")

    def test_a_named_pass_error_is_the_diagnostic(self) -> None:
        stderr = (
            "odin3: error: dfflibmap: module 'RefModule': no cell of library 'cmos6' "
            "implements $_DLATCH_P_ 'p.dlatch_p0' (high, INIT 3)\n"
            "odin3: error: pass dfflibmap: failed: ODIN3_ERR_INVALID_ARG\n"
            "odin3: error: -p: command 7: pass 'dfflibmap' failed: ODIN3_ERR_INVALID_ARG\n"
        )
        verdict = veval_coverage.classify(1, stderr)
        self.assertEqual(verdict.kind, "refused")
        self.assertEqual(verdict.refusal, "other")
        self.assertIn("no cell of library 'cmos6' implements $_DLATCH_P_", verdict.message)


class ManifestTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.refs = self.dir / "refs"
        self.refs.mkdir()
        for name in ("Prob001_zero_ref.sv", "Prob151_review2015_fsm_ref.sv"):
            (self.refs / name).write_text("module RefModule; endmodule\n")
        self.manifest = self.dir / "manifest.tsv"
        self.odin3 = self.fake_odin3()

    def fake_odin3(self) -> Path:
        path = self.dir / "odin3"
        path.write_text(FAKE_ODIN3.format(python=sys.executable))
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def check(self, modes: dict[str, str]) -> tuple[int, str]:
        """Run the manifest check with one fake mode per design (default good)."""
        old = {
            key: os.environ.get(key)
            for key in list(os.environ)
            if key.startswith("VVEVAL_FAKE")
        }
        for key in old:
            del os.environ[key]
        for design, mode in modes.items():
            os.environ["VVEVAL_FAKE_" + design] = mode
        try:
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                code, _outcomes = veval_coverage.check_manifest(
                    self.odin3, self.refs, self.odin3, self.dir, 30, self.manifest
                )
            return code, err.getvalue()
        finally:
            for key in [key for key in os.environ if key.startswith("VVEVAL_FAKE")]:
                del os.environ[key]
            for key, value in old.items():
                if value is not None:
                    os.environ[key] = value

    def test_matching_manifest_passes(self) -> None:
        self.manifest.write_text(
            "Prob001_zero_ref.sv\tok\n"
            "Prob151_review2015_fsm_ref.sv\trefused:explicit_cast\t"
            "an explicit 4-bit cast is not supported\n"
        )
        code, err = self.check(
            {"Prob151_review2015_fsm_ref.sv": "refuse:an explicit 4-bit cast is not supported"}
        )
        self.assertEqual(code, 0, err)

    def test_an_ok_design_regressing_to_refusal_fails(self) -> None:
        self.manifest.write_text(
            "Prob001_zero_ref.sv\tok\n"
            "Prob151_review2015_fsm_ref.sv\trefused:explicit_cast\t"
            "an explicit 4-bit cast is not supported\n"
        )
        # The fake refuses the ok design, so the ok row regresses.
        code, err = self.check(
            {
                "Prob001_zero_ref.sv": "refuse:an explicit 4-bit cast is not supported",
                "Prob151_review2015_fsm_ref.sv": "refuse:an explicit 4-bit cast is not supported",
            }
        )
        self.assertEqual(code, 1)
        self.assertIn("Prob001_zero_ref.sv: expected ok", err)

    def test_a_crash_fails(self) -> None:
        self.manifest.write_text(
            "Prob001_zero_ref.sv\tok\n"
            "Prob151_review2015_fsm_ref.sv\trefused:explicit_cast\t"
            "an explicit 4-bit cast is not supported\n"
        )
        code, err = self.check({"Prob151_review2015_fsm_ref.sv": "crash"})
        self.assertEqual(code, 1)
        self.assertIn("crash", err)

    def test_a_changed_refusal_class_fails(self) -> None:
        self.manifest.write_text(
            "Prob001_zero_ref.sv\tok\n"
            "Prob151_review2015_fsm_ref.sv\trefused:explicit_cast\t"
            "an explicit 4-bit cast is not supported\n"
        )
        code, err = self.check(
            {"Prob151_review2015_fsm_ref.sv": "refuse:ref port 'x' is not supported"}
        )
        self.assertEqual(code, 1)
        self.assertIn("expected class explicit_cast, got ref_port", err)

    def test_a_changed_pinned_message_fails(self) -> None:
        self.manifest.write_text(
            "Prob001_zero_ref.sv\tok\n"
            "Prob151_review2015_fsm_ref.sv\trefused:explicit_cast\t"
            "an explicit 4-bit cast is not supported\n"
        )
        code, err = self.check(
            {"Prob151_review2015_fsm_ref.sv": "refuse:an explicit 8-bit cast is not supported"}
        )
        self.assertEqual(code, 1)
        self.assertIn("expected pinned message", err)

    def test_missing_and_extra_rows_fail(self) -> None:
        self.manifest.write_text("Prob001_zero_ref.sv\tok\nProb999_missing_ref.sv\tok\n")
        code, err = self.check({})
        self.assertEqual(code, 1)
        self.assertIn("Prob151_review2015_fsm_ref.sv: in the corpus but not in the manifest", err)
        self.assertIn("Prob999_missing_ref.sv: in the manifest but not in the corpus", err)

    def test_bad_manifest_rows_are_refused(self) -> None:
        for text in (
            "Prob001_zero_ref.sv\trefused\n",
            "Prob001_zero_ref.sv\trefused:explicit_cast\n",
            "Prob001_zero_ref.sv\tok\tpinned\n",
            "Prob001_zero_ref.sv\trefused:made_up\tx\n",
        ):
            self.manifest.write_text(text)
            with self.assertRaises(SystemExit):
                veval_coverage.load_manifest(self.manifest)


class WriteManifestTest(unittest.TestCase):
    def test_a_crash_is_never_pinned(self) -> None:
        outcomes = [
            (Path("Prob001_zero_ref.sv"), veval_coverage.Verdict("crash", detail="signal 11"))
        ]
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(SystemExit):
            veval_coverage.write_manifest(outcomes, Path(tmp) / "manifest.tsv")

    def test_rows_round_trip(self) -> None:
        outcomes = [
            (Path("Prob001_zero_ref.sv"), veval_coverage.Verdict("ok")),
            (
                Path("Prob151_review2015_fsm_ref.sv"),
                veval_coverage.Verdict(
                    "refused",
                    refusal="explicit_cast",
                    message="an explicit 4-bit cast is not supported",
                ),
            ),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "manifest.tsv"
            veval_coverage.write_manifest(outcomes, path)
            self.assertEqual(
                veval_coverage.load_manifest(path),
                [
                    veval_coverage.ManifestEntry("Prob001_zero_ref.sv", "ok"),
                    veval_coverage.ManifestEntry(
                        "Prob151_review2015_fsm_ref.sv",
                        "refused:explicit_cast",
                        "an explicit 4-bit cast is not supported",
                    ),
                ],
            )


if __name__ == "__main__":
    unittest.main()

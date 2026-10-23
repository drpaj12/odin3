"""veval-coverage: Odin III coverage over the VerilogEval spec-to-RTL references.

Every ``*_ref.sv`` in a reference directory runs through the SystemVerilog front end and the
gate flow (``read_techlib``; ``read_verilog``; ``proc``; ``opt``; ``lower``;
``abc --gates cmos6``; ``dfflibmap cmos6``; ``write_blif``) and the outcome is classified:

- ``ok``: the flow finished and wrote a BLIF.
- ``refused``: the front end or a pass refused the design with a located error. The message
  names a refusal class: ``unique_or_priority``, ``explicit_cast``, ``intra_assignment_timing``,
  ``drive_strength``, ``ref_port``, ``union_member`` or ``other``.
- ``crash``: a signal, a sanitizer report or a timeout. That is a bug to report, never an
  expected outcome.

Two modes share the same run and classification:

- Default coverage mode runs every reference, prints one line per design, the per-class counts
  and the top refused messages, and writes the full result as JSON with ``--json``.
  ``--manifest-out`` writes the manifest the corpus test consumes.
- ``--manifest FILE`` checks a pinned manifest instead: every ``ok`` design must still pass and
  every refused design must refuse with the same class and a message that contains its pinned
  text. Any mismatch, any crash and any corpus file missing from the manifest fails.

``--equiv-sample N --yosys YOSYS`` additionally checks up to N ``ok`` designs (smallest first)
against the Yosys oracle's techmapped BLIF with ``tools/equiv-check``, as
``tests/cli/slang_flow.sh`` does. Both the gate BLIF the flow wrote (the corpus artifact) and
the un-gated RTL BLIF are compared, so a gate-mapping regression that still writes a netlist
fails.

Exit 0 all expected, 1 a crash, a mismatch or an input error (an empty reference directory, a
malformed manifest, a crash to pin), 2 a usage error (a bad option or a missing odin3, techlib
or equiv-check). See ``tests/cli/veval_corpus.sh`` for the CTest entry.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

TOP = "RefModule"
GATE_FLOW = "proc; opt; lower; abc --gates cmos6; dfflibmap cmos6"
REF_SUFFIX = "_ref.sv"

# The located error the CLI prints: "odin3: error: read_slang: FILE:LINE:COL: TEXT" (other
# passes may leave out the "read_slang: " tag; the pinned text is always after the location).
_ERROR = re.compile(r"^odin3: error: (?P<text>.*)$", re.MULTILINE)
_LOCATION = re.compile(r"^(?:read_slang: )?(?:[^:\s]+:\d+:\d+: )?(?P<message>.*)$")
# The pass runner's summary lines: they repeat the status and hide the diagnostic an earlier
# error line carries (read_slang's located text, dfflibmap's named cell), so they are skipped.
_GENERIC = re.compile(
    r"^(?:-p: command .*: pass '.*' failed: ODIN3_ERR_\w+|pass \w+: failed: ODIN3_ERR_\w+)$"
)

# Refusal kinds, in match order. Earlier patterns win (an explicit cast is not "other").
_REFUSALS: tuple[tuple[str, re.Pattern[str]], ...] = (
    ("unique_or_priority", re.compile(r"'unique0? (?:case|if)' is not supported")),
    ("unique_or_priority", re.compile(r"'priority (?:case|if)' is not supported")),
    ("explicit_cast", re.compile(r"an explicit \d+-bit cast is not supported")),
    ("intra_assignment_timing", re.compile(r"an intra-assignment timing control is not supported")),
    ("drive_strength", re.compile(r"a drive strength on (?:net|a continuous assignment) ")),
    ("ref_port", re.compile(r"(?:ref port|ref modport port) '[^']*' is not supported")),
    ("union_member", re.compile(r"member '[^']*' of a type with no bit layout")),
)
_CRASH_MARKERS = (
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
    "runtime error:",
    "LeakSanitizer",
    "Assertion `",
    "odin3 check failed",
)

CLASSES = (
    "unique_or_priority",
    "explicit_cast",
    "intra_assignment_timing",
    "drive_strength",
    "ref_port",
    "union_member",
    "other",
)


@dataclass(frozen=True)
class Verdict:
    """One design's outcome: kind is "ok", "refused" or "crash"."""

    kind: str
    refusal: str = ""
    message: str = ""
    detail: str = ""


@dataclass(frozen=True)
class ManifestEntry:
    """One manifest row: a design, its expected kind and (for a refusal) its pinned text."""

    design: str
    expected: str
    pinned: str = ""


def diagnostic(stderr: str) -> str:
    """The most specific ``odin3: error:`` text, else the last non-empty line.

    A pass that fails logs the real diagnostic and then a generic "pass ... failed" summary;
    the first non-summary error is the one worth pinning.
    """
    errors = [str(error).strip() for error in _ERROR.findall(stderr)]
    specific = [error for error in errors if not _GENERIC.match(error)]
    if specific:
        return specific[0]
    if errors:
        return errors[-1]
    lines = [line.strip() for line in stderr.splitlines() if line.strip()]
    return lines[-1] if lines else ""


def pinned_text(error: str) -> str:
    """The diagnostic without the ``read_slang:`` tag and the file:line:column location."""
    match = _LOCATION.match(error)
    return (match.group("message") if match else error).strip()


def refusal_class(error: str) -> str:
    """The refusal class of a located message; "other" when no known class matches."""
    for name, pattern in _REFUSALS:
        if pattern.search(error):
            return name
    return "other"


def classify(returncode: int | None, stderr: str) -> Verdict:
    """Classify one run: ok, refused (with class and message) or crash (with detail).

    ``returncode`` is the process exit code, or None for a timeout (``run_odin3``'s marker);
    a negative value is the signal that killed the process.
    """
    if any(marker in stderr for marker in _CRASH_MARKERS):
        return Verdict("crash", detail=diagnostic(stderr) or "sanitizer report")
    if returncode is None:
        return Verdict("crash", detail=diagnostic(stderr) or "timed out")
    if returncode < 0:
        return Verdict("crash", detail=f"killed by signal {-returncode}")
    error = diagnostic(stderr)
    if returncode != 0:
        if not error:
            return Verdict("crash", detail=f"exit {returncode} with no diagnostic")
        return Verdict("refused", refusal=refusal_class(error), message=pinned_text(error))
    return Verdict("ok")


def quote(path: Path | str) -> str:
    """A path as one pass-script word (the CLI's parser accepts double quotes)."""
    return '"' + str(path).replace('"', '\\"') + '"'


def run_odin3(odin3: Path, script: str, timeout: float) -> tuple[int | None, str]:
    """Run odin3 with one -p script; return (exit code, stderr), None for a timeout."""
    try:
        done = subprocess.run(
            [str(odin3), "--top", TOP, "-p", script],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return None, f"odin3: error: timed out after {timeout:.0f} s"
    return done.returncode, done.stderr


def run_flow(odin3: Path, ref: Path, lib: Path, out: Path, timeout: float) -> Verdict:
    """One reference through read_verilog and the gate flow, ending in write_blif."""
    script = (
        f"read_techlib {quote(lib)}; read_verilog {quote(ref)}; {GATE_FLOW}; "
        f"write_blif --gates-as-names {quote(out)}"
    )
    returncode, stderr = run_odin3(odin3, script, timeout)
    verdict = classify(returncode, stderr)
    if verdict.kind == "ok" and (not out.is_file() or out.stat().st_size == 0):
        return Verdict("crash", detail="the flow succeeded but wrote no BLIF")
    return verdict


def run_rtl_blif(odin3: Path, ref: Path, out: Path, timeout: float) -> tuple[int | None, str]:
    """The un-gated BLIF (read_verilog; proc; opt; lower) the RTL leg compares."""
    script = (
        f"read_verilog {quote(ref)}; proc; opt; lower; "
        f"write_blif --gates-as-names {quote(out)}"
    )
    return run_odin3(odin3, script, timeout)


def references(refs: Path) -> list[Path]:
    """The ``*_ref.sv`` files in the reference directory, sorted by name."""
    found = sorted(refs.glob("*" + REF_SUFFIX))
    if not found:
        raise SystemExit(f"veval-coverage: no *{REF_SUFFIX} files in {refs}")
    return found


def coverage(
    odin3: Path, refs: Path, lib: Path, work: Path, timeout: float
) -> tuple[list[tuple[Path, Verdict]], int]:
    """Run every reference; return the outcomes (in name order) and the crash count."""
    results: list[tuple[Path, Verdict]] = []
    crashes = 0
    for ref in references(refs):
        out = work / (ref.name[: -len(".sv")] + ".blif")
        verdict = run_flow(odin3, ref, lib, out, timeout)
        results.append((ref, verdict))
        crashes += verdict.kind == "crash"
        print(f"{ref.name}\t{describe(verdict)}")
    return results, crashes


def describe(verdict: Verdict) -> str:
    """One-line description of a verdict."""
    if verdict.kind == "ok":
        return "ok"
    if verdict.kind == "refused":
        return f"refused\t{verdict.refusal}\t{verdict.message}"
    return f"crash\t{verdict.detail}"


def summary(results: list[tuple[Path, Verdict]], total: int) -> None:
    """The coverage table: counts per outcome and class, the top refused messages."""
    kinds = Counter(verdict.kind for _, verdict in results)
    print(f"\nveval-coverage: {total} references")
    print(f"  ok      {kinds['ok']}")
    print(f"  refused {kinds['refused']}")
    print(f"  crash   {kinds['crash']}")
    classes = Counter(verdict.refusal for _, verdict in results if verdict.kind == "refused")
    for name in CLASSES:
        if classes[name]:
            print(f"    {name}: {classes[name]}")
    messages = Counter(
        verdict.message for _, verdict in results if verdict.kind == "refused"
    )
    if messages:
        print("  top refused messages")
        for message, count in messages.most_common(10):
            print(f"    {count:3d}  {message}")


def write_json(results: list[tuple[Path, Verdict]], path: Path) -> None:
    """The full classification as JSON: one object per design."""
    rows = [
        {
            "design": ref.name,
            "kind": verdict.kind,
            "refusal": verdict.refusal,
            "message": verdict.message,
            "detail": verdict.detail,
        }
        for ref, verdict in results
    ]
    path.write_text(json.dumps(rows, indent=1) + "\n")


def write_manifest(results: list[tuple[Path, Verdict]], path: Path) -> None:
    """The manifest the corpus test consumes: design, expected kind, pinned refusal text."""
    lines = [
        "# VerilogEval corpus outcomes (tools/veval/veval-coverage --manifest-out).",
        "# design<TAB>ok | refused:<class><TAB>pinned refusal message",
        "# A crash is a bug: there is no expected-crash row.",
    ]
    for ref, verdict in results:
        if verdict.kind == "ok":
            lines.append(f"{ref.name}\tok")
        elif verdict.kind == "refused":
            lines.append(f"{ref.name}\trefused:{verdict.refusal}\t{verdict.message}")
        else:
            raise SystemExit(
                f"veval-coverage: refusing to pin a crash: {ref.name}: {verdict.detail}"
            )
    path.write_text("\n".join(lines) + "\n")


def load_manifest(path: Path) -> list[ManifestEntry]:
    """Parse a manifest; blank lines and ``#`` comments are skipped."""
    entries: list[ManifestEntry] = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) not in (2, 3):
            raise SystemExit(f"veval-coverage: {path}:{number}: expected 2 or 3 tab fields")
        design, expected = fields[0], fields[1]
        if expected != "ok" and not (
            expected.startswith("refused:")
            and expected[len("refused:") :] in CLASSES
        ):
            raise SystemExit(f"veval-coverage: {path}:{number}: bad expectation {expected!r}")
        pinned = fields[2] if len(fields) == 3 else ""
        if expected == "ok" and pinned:
            raise SystemExit(f"veval-coverage: {path}:{number}: an ok row takes no pinned text")
        if expected != "ok" and not pinned:
            raise SystemExit(f"veval-coverage: {path}:{number}: a refused row needs pinned text")
        entries.append(ManifestEntry(design, expected, pinned))
    return entries


def check_manifest(
    odin3: Path, refs: Path, lib: Path, work: Path, timeout: float, manifest: Path
) -> tuple[int, list[tuple[Path, Verdict]]]:
    """Run every reference against its pinned outcome; print every mismatch.

    Returns the exit status and the outcomes, so ``--equiv-sample`` can reuse them.
    """
    entries = load_manifest(manifest)
    listed = {entry.design for entry in entries}
    corpus = {ref.name for ref in references(refs)}
    failures: list[str] = []
    for missing in sorted(corpus - listed):
        failures.append(f"{missing}: in the corpus but not in the manifest")
    for extra in sorted(listed - corpus):
        failures.append(f"{extra}: in the manifest but not in the corpus")
    if len(entries) != len(listed):
        failures.append(f"{manifest}: duplicate design rows")
    outcomes: list[tuple[Path, Verdict]] = []
    for entry in entries:
        ref = refs / entry.design
        if not ref.is_file():
            continue
        out = work / (ref.name[: -len(".sv")] + ".blif")
        verdict = run_flow(odin3, ref, lib, out, timeout)
        outcomes.append((ref, verdict))
        want = entry.expected
        if want == "ok":
            if verdict.kind != "ok":
                failures.append(f"{ref.name}: expected ok, got {describe(verdict)}")
            continue
        expected_class = want[len("refused:") :]
        if verdict.kind != "refused":
            failures.append(f"{ref.name}: expected {want}, got {describe(verdict)}")
        elif verdict.refusal != expected_class:
            failures.append(
                f"{ref.name}: expected class {expected_class}, got {verdict.refusal}: "
                f"{verdict.message}"
            )
        elif entry.pinned not in verdict.message:
            failures.append(
                f"{ref.name}: expected pinned message {entry.pinned!r}, got {verdict.message!r}"
            )
    if failures:
        for failure in failures:
            print(f"veval-coverage: FAIL: {failure}", file=sys.stderr)
        return 1, outcomes
    print(f"veval-coverage: {len(entries)} designs match the manifest")
    return 0, outcomes


def _equiv_one(
    odin3: Path, yosys: Path, equiv: Path, ref: Path, work: Path, timeout: float
) -> tuple[bool, str]:
    """One design against the Yosys oracle: its gate BLIF and its RTL BLIF.

    The gate BLIF is the corpus artifact the manifest pins; ``run_flow`` already wrote it to
    work/<stem>.blif, so a gate-mapping regression that still writes a netlist fails here. The
    RTL BLIF is the second leg: when it differs, the front end or a pre-gate pass is the cause.
    """
    stem = ref.name[: -len(".sv")]
    rtl_blif = work / (stem + ".odin.blif")
    gate_blif = work / (stem + ".blif")
    yosys_blif = work / (stem + ".yosys.blif")
    if not gate_blif.is_file() or gate_blif.stat().st_size == 0:
        return False, f"the flow wrote no gate BLIF at {gate_blif}"
    returncode, stderr = run_rtl_blif(odin3, ref, rtl_blif, timeout)
    if returncode != 0:
        return False, f"odin3 RTL BLIF failed: {diagnostic(stderr)}"
    script = (
        f"read_verilog -sv {quote(ref)}; hierarchy -top {TOP}; proc; opt; techmap; opt; "
        f"write_blif {quote(yosys_blif)}"
    )
    try:
        oracle = subprocess.run(
            [str(yosys), "-q", "-p", script],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return False, "Yosys timed out"
    if oracle.returncode != 0:
        return False, f"Yosys failed: {diagnostic(oracle.stderr)}"
    for leg, blif in (("RTL", rtl_blif), ("gate", gate_blif)):
        try:
            checked = subprocess.run(
                [str(equiv), str(blif), str(yosys_blif)],
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return False, f"equiv-check timed out on the {leg} BLIF"
        if checked.returncode != 0:
            detail = diagnostic(checked.stdout + checked.stderr) or "not equivalent"
            return False, f"{leg} BLIF differs from Yosys: {detail}"
    return True, "RTL and gate BLIFs equivalent to Yosys"


def equiv_sample(
    odin3: Path,
    outcomes: list[tuple[Path, Verdict]],
    work: Path,
    timeout: float,
    sample: int,
    yosys: Path,
    equiv: Path,
) -> int:
    """The up-to-`sample` smallest ok references against the Yosys oracle."""
    ok = [ref for ref, verdict in outcomes if verdict.kind == "ok"]
    ok.sort(key=lambda ref: (ref.stat().st_size, ref.name))
    chosen = ok[:sample]
    print(f"\nveval-coverage: equivalence sample: {len(chosen)} of {len(ok)} ok references")
    failed = 0
    for ref in chosen:
        passed, detail = _equiv_one(odin3, yosys, equiv, ref, work, timeout)
        failed += not passed
        print(f"{ref.name}\t{'ok' if passed else 'FAIL'}\t{detail}")
    print(f"veval-coverage: equivalence sample: {len(chosen) - failed} ok, {failed} failed")
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="veval-coverage",
        description="Classify the VerilogEval refs through Odin III's SystemVerilog gate flow.",
    )
    parser.add_argument("--odin3", type=Path, required=True, help="the odin3 executable")
    parser.add_argument("--refs", type=Path, required=True, help="a directory of *_ref.sv")
    parser.add_argument("--lib", type=Path, help="the cmos6 techlib (default: lib/cmos6.o3lib)")
    parser.add_argument("--work", type=Path, help="scratch directory (default: a temporary one)")
    parser.add_argument("--timeout", type=float, default=120.0, help="seconds per design")
    parser.add_argument("--json", type=Path, help="write the full classification here")
    parser.add_argument("--manifest", type=Path, help="check this pinned manifest instead")
    parser.add_argument("--manifest-out", type=Path, help="write the manifest here")
    parser.add_argument("--equiv-sample", type=int, default=0, help="check N ok designs vs Yosys")
    parser.add_argument("--yosys", type=Path, help="the Yosys oracle (required by --equiv-sample)")
    parser.add_argument("--equiv-check", type=Path, help="tools/equiv-check/equiv-check")
    args = parser.parse_args(argv)

    root = Path(__file__).resolve().parents[2]
    lib = args.lib or root / "lib" / "cmos6.o3lib"
    equiv = args.equiv_check or root / "tools" / "equiv-check" / "equiv-check"
    if not args.odin3.is_file():
        parser.error(f"no odin3 executable: {args.odin3}")
    if not lib.is_file():
        parser.error(f"no techlib: {lib}")
    if args.equiv_sample:
        if args.yosys is None or not args.yosys.is_file():
            parser.error("--equiv-sample needs --yosys naming the executable")
        if not equiv.is_file():
            parser.error(f"no equiv-check: {equiv}")

    own_work = args.work is None
    work = args.work or Path(tempfile.mkdtemp(prefix="veval-coverage."))
    work.mkdir(parents=True, exist_ok=True)
    try:
        if args.manifest is not None:
            status, outcomes = check_manifest(
                args.odin3, args.refs, lib, work, args.timeout, args.manifest
            )
        else:
            outcomes, crashes = coverage(args.odin3, args.refs, lib, work, args.timeout)
            summary(outcomes, len(outcomes))
            if args.json is not None:
                write_json(outcomes, args.json)
            if args.manifest_out is not None:
                write_manifest(outcomes, args.manifest_out)
            status = 1 if crashes else 0
        if args.equiv_sample:
            sample = equiv_sample(
                args.odin3,
                outcomes,
                work,
                args.timeout,
                args.equiv_sample,
                args.yosys if args.yosys is not None else Path("yosys"),
                equiv,
            )
            status = max(status, sample)
        return status
    finally:
        if own_work:
            for path in sorted(work.glob("*"), reverse=True):
                path.unlink(missing_ok=True)
            work.rmdir()


if __name__ == "__main__":
    sys.exit(main())

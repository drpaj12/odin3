"""qor_parse: VTR's own parse, ``time -v`` and the stage costs (3H spec QH-4.2 to QH-4.4).

Three readers feed ``metrics.json`` (spec §5.2):

* VTR's ``parse_vtr_flow.py`` with the ``vpr_standard`` and ``qor_standard`` configs, exactly the
  per-job parse ``run_vtr_task.py`` performs (``parse_commands``, ``read_parse_results``,
  ``metrics_from_parse``). The values are VTR's.
* GNU ``time -v`` output (``read_time_v``): ``frontend.time`` for the Odin III legs, and the
  stage logs VTR writes with ``-track_memory_usage`` (``stage_costs``).
* ``vpr.out`` and ``vpr.crit_path.out`` for the summed VPR time (``vpr_time_s``).

A metric that is not there is ``None``, never ``-1``: VTR prints ``-1`` for a key its patterns
did not match, and a missing gated metric must fail the gate explicitly (spec §6.2).
"""

from __future__ import annotations

import math
import re
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from qor_heavy import HeavyError
from qor_legs import Command

PARSE_SCRIPT_REL = Path("vtr_flow") / "scripts" / "python_libs" / "vtr" / "parse_vtr_flow.py"
PYTHON_LIBS_REL = Path("vtr_flow") / "scripts" / "python_libs"
VPR_CONFIG_REL = Path("vtr_flow") / "parse" / "parse_config" / "vpr_standard.txt"
QOR_CONFIG_REL = Path("vtr_flow") / "parse" / "qor_config" / "qor_standard.txt"
PARSE_RESULTS = "parse_results.txt"
QOR_RESULTS = "qor_results.txt"
# VTR prints this for a key none of its patterns matched (log_parse: the default value).
ABSENT = "-1"
KIB_PER_MIB = 1024
HALF = 0.5
MHZ_PER_NS = 1000.0
# VTR's default stage logs (spec §4.4): parmys, the ABC stage per clock domain and its helpers.
STAGE_GLOBS = (
    "parmys.out",
    "abc*.out",
    "*_blackboxing_latch.out",
    "restore_latch*.out",
    "report_clocks.abc.out",
)
VPR_LOGS = ("vpr.out", "vpr.crit_path.out")

_NUMBER = re.compile(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?")
_ELAPSED_LINE = re.compile(r"^\s*Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): (\S+)\s*$")
_ELAPSED_VALUE = re.compile(r"(?:(\d+):)?(\d+):(\d+(?:\.\d*)?)")
_MAXRSS_LINE = re.compile(r"^\s*Maximum resident set size \(kbytes\): (\d+)\s*$")
# VTR matches its patterns as ^.*<regex>.*$ (log_parse.ParsePattern).
_VPR_FLOW_TIME = re.compile(r"^.*The entire flow of VPR took (.*) seconds.*$")

MetricValue = float | int | str | None


def parse_commands(
    vtr_root: Path, python: Path, run_dir: Path, arch_xml: Path, circuit: str
) -> list[Command]:
    """The two parse commands of spec §4.2: ``vpr_standard`` into ``parse_results.txt`` and
    ``qor_standard`` into ``qor_results.txt`` (both in ``run_dir``)."""
    libs = vtr_root / PYTHON_LIBS_REL
    commands = []
    for config, out in ((VPR_CONFIG_REL, PARSE_RESULTS), (QOR_CONFIG_REL, QOR_RESULTS)):
        argv = (
            str(python),
            str(vtr_root / PARSE_SCRIPT_REL),
            str(run_dir),
            str(vtr_root / config),
            f"arch={arch_xml.name}",
            f"circuit={circuit}",
            "script_params=common",
        )
        commands.append(Command(argv, run_dir, run_dir / out, (("PYTHONPATH", str(libs)),)))
    return commands


def _split_row(line: str) -> list[str]:
    """One tab-separated row; ``parse_vtr_flow.py`` ends every row with a tab."""
    cells = line.rstrip("\r").split("\t")
    if cells and cells[-1] == "":
        cells.pop()
    return cells


def read_parse_results(path: Path, *, not_before: float, label: str) -> dict[str, str]:
    """``parse_vtr_flow.py``'s output: one header row, one value row, as a name → text map.

    A file older than ``not_before`` is a leftover of an earlier attempt (E-QH8). A missing or
    malformed file is also a failed job; the values are never guessed."""
    try:
        mtime = path.stat().st_mtime
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as err:
        raise HeavyError(1, f"{label}: cannot read {path.name}: {err.strerror}") from err
    if mtime < not_before:
        raise HeavyError(1, f"{label}: stale {path.name} from an earlier attempt; job failed")
    lines = text.split("\n")
    while lines and not lines[-1].strip():
        lines.pop()
    if len(lines) != 2:
        raise HeavyError(1, f"{label}: malformed {path.name}; job failed")
    names, values = _split_row(lines[0]), _split_row(lines[1])
    if len(names) != len(values) or len(set(names)) != len(names):
        raise HeavyError(1, f"{label}: malformed {path.name}; job failed")
    return dict(zip(names, values, strict=True))


def present(text: str | None) -> float | None:
    """The number in ``text`` when it is finite and not VTR's ``-1`` stand-in, else ``None``."""
    if text is None:
        return None
    stripped = text.strip()
    if _NUMBER.fullmatch(stripped) is None:
        return None
    value = float(stripped)
    if not math.isfinite(value) or value == float(ABSENT):
        return None
    return value


@dataclass(frozen=True)
class TimeV:
    """What GNU ``time -v`` reported: ``None`` for a figure the file does not carry."""

    wall_s: float | None
    peak_rss_mib: int | None


def _elapsed_seconds(text: str) -> float | None:
    match = _ELAPSED_VALUE.fullmatch(text)
    if match is None:
        return None
    hours, minutes, seconds = match.groups()
    return int(hours or 0) * 3600 + int(minutes) * 60 + float(seconds)


def read_time_v(path: Path) -> TimeV:
    """Wall seconds and peak RSS (MiB, rounded half up) from the ``time -v`` blocks in ``path``.

    Several blocks (a log that holds more than one timed command) add their wall times and take
    the largest peak. A file that is missing or carries no such lines gives ``None``."""
    wall: float | None = None
    peak_kib: int | None = None
    try:
        with path.open(encoding="utf-8", errors="replace") as handle:
            for line in handle:
                elapsed = _ELAPSED_LINE.match(line)
                if elapsed is not None:
                    seconds = _elapsed_seconds(elapsed.group(1))
                    if seconds is not None:
                        wall = seconds if wall is None else wall + seconds
                    continue
                rss = _MAXRSS_LINE.match(line)
                if rss is not None:
                    kib = int(rss.group(1))
                    peak_kib = kib if peak_kib is None else max(peak_kib, kib)
    except OSError:
        return TimeV(None, None)
    peak = None if peak_kib is None else (peak_kib + KIB_PER_MIB // 2) // KIB_PER_MIB
    return TimeV(wall, peak)


def vpr_time_s(run_dir: Path) -> float | None:
    """The sum of every ``The entire flow of VPR took N seconds`` in ``vpr.out`` and
    ``vpr.crit_path.out`` (spec §4.2), or ``None`` when there is none or one is unreadable."""
    total = 0.0
    found = False
    for name in VPR_LOGS:
        try:
            with (run_dir / name).open(encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    match = _VPR_FLOW_TIME.match(line.rstrip("\n"))
                    if match is None:
                        continue
                    seconds = present(match.group(1))
                    if seconds is None:
                        return None
                    total += seconds
                    found = True
        except FileNotFoundError:
            continue
        except OSError:
            return None
    return total if found else None


@dataclass(frozen=True)
class StageCost:
    """The summed cost of VTR's front-end stages: ``complete`` is false when a log lacks its
    ``time -v`` lines (or no log matched), and then ``warnings`` says which."""

    wall_s: float
    peak_rss_mib: int
    complete: bool
    warnings: tuple[str, ...]


def stage_costs(run_dir: Path, globs: Sequence[str], label: str) -> StageCost:
    """Wall times summed and peak RSS maximised over every log in ``run_dir`` the ``globs``
    match (each file once). A log without ``time -v`` lines is named in a warning (spec §4.4)."""
    files = sorted({p for pattern in globs for p in run_dir.glob(pattern) if p.is_file()})
    wall = 0.0
    peak = 0
    warnings: list[str] = []
    for log in files:
        timing = read_time_v(log)
        if timing.wall_s is None or timing.peak_rss_mib is None:
            warnings.append(f"vtr-qor: {label}: no time -v in {log.name}; stage cost incomplete")
        if timing.wall_s is not None:
            wall += timing.wall_s
        if timing.peak_rss_mib is not None:
            peak = max(peak, timing.peak_rss_mib)
    if not files:
        warnings.append(f"vtr-qor: {label}: no stage logs in {run_dir}; stage cost incomplete")
    return StageCost(wall, peak, not warnings, tuple(warnings))


def _count(text: str | None) -> int | None:
    value = present(text)
    return int(value) if value is not None and value.is_integer() else None


def _text(text: str | None) -> str | None:
    if text is None:
        return None
    stripped = text.strip()
    return stripped if stripped and stripped != ABSENT else None


def _mem_mib(text: str | None) -> int | None:
    """``max_vpr_mem`` (kB) in MiB, rounded half up."""
    kib = present(text)
    return None if kib is None else math.floor(kib / KIB_PER_MIB + HALF)


def _fmax_mhz(delay_ns: float | None) -> float | None:
    return MHZ_PER_NS / delay_ns if delay_ns is not None and delay_ns > 0 else None


def metrics_from_parse(
    parse: Mapping[str, str], qor: Mapping[str, str]
) -> dict[str, dict[str, MetricValue]]:
    """§5.2's ``packed``, ``routed`` and the VPR part of ``cost`` by §4.2's table.

    ``parse`` is ``parse_results.txt`` and ``qor`` is ``qor_results.txt``. An absent key, VTR's
    ``-1`` or an unreadable value is ``None``."""
    delay = present(parse.get("critical_path_delay"))
    return {
        "packed": {
            "num_clb": _count(parse.get("num_clb")),
            "num_memories": _count(parse.get("num_memories")),
            "num_mult": _count(parse.get("num_mult")),
            "device_width": _count(parse.get("device_width")),
            "device_height": _count(parse.get("device_height")),
        },
        "routed": {
            "vpr_status": _text(parse.get("vpr_status")),
            "min_chan_width": _count(parse.get("min_chan_width")),
            "critical_path_delay_ns": delay,
            "fmax_mhz": _fmax_mhz(delay),
            "crit_path_routed_wirelength": _count(parse.get("crit_path_routed_wirelength")),
        },
        "cost": {
            "max_vpr_mem_mib": _mem_mib(parse.get("max_vpr_mem")),
            "vpr_total_runtime_s": present(qor.get("total_runtime")),
        },
    }

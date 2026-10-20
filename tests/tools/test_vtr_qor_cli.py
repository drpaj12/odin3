"""Tests for tools/vtr-qor/vtr_qor.py, the vtr-qor command line (3H plan Task 6).

Every case builds its fakes under /tmp/claude-1000: a VTR tree that is a git repository at a chosen
commit (with a fake ``run_vtr_flow.py``, ``parse_vtr_flow.py``, ``vtr/paths.py`` and binaries),
a fake ``odin3`` with its ``--help``, ``time``, ``ldd``, a cgroup tree standing for heavy-run's
slice and scope, descriptor 9 on a fake lock, a fake ``df`` and ``meminfo``. They are reached
through module seams (``mock.patch.object``), never an environment hook. Nothing runs VTR, takes
the real heavy lock or reads the real cgroups.

``T8Check`` is the manual check of T8 (the first real 3-design P run): skipped unless
``ODIN3_QOR_T8=1``; it only reads the finished run's files.
"""

from __future__ import annotations

import fcntl
import fnmatch
import hashlib
import json
import os
import pwd
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import ExitStack
from pathlib import Path
from typing import Any
from unittest import mock

import qor_heavy
import qor_legs
import qor_parse
import qor_sched
import qor_table
import vtr_qor
from qor_heavy import HEAVY_DIR

from .helpers import REPO_ROOT, run_main, write_golden

K6 = "k6_frac_N10_frac_chain_mem32K_40nm"
SETS = qor_sched.load_sets()
GIB = 1024**3
KIB_PER_GIB = 1024**2
SLICE_DIRS = "user.slice/user-1000.slice/user@1000.service/odin3.slice"
CLI = REPO_ROOT / "tools" / "vtr-qor" / "vtr_qor.py"
SHIM = REPO_ROOT / "tools" / "vtr-qor" / "vtr-qor"
ALL_PASSES = ("read_techlib", "read_verilog", "read_blif", "proc", "fsm", "opt", "flatten",
              "wreduce", "hardmap", "lower", "abc", "write_blif")
TIMEV = ("\tElapsed (wall clock) time (h:mm:ss or m:ss): 0:00.50\n"
         "\tMaximum resident set size (kbytes): 20480\n")
BLIF = (".model top\n.inputs a b clk\n.outputs y\n.names a b n1\n11 1\n"
        ".latch n1 y re clk 0\n.end\n")
ABC_LINE = (f"# abc: script=lib/abc/vtr.abc sha256={'1' * 64} abc={'2' * 40} K=6 "
            "exchange=aig depth=2\n")
VPR_FIELDS = {"vpr_status": "success", "num_clb": "12", "num_memories": "1", "num_mult": "2",
              "device_width": "8", "device_height": "9", "min_chan_width": "40",
              "critical_path_delay": "2.5", "crit_path_routed_wirelength": "1234",
              "max_vpr_mem": "65536", "abc_depth": "3"}

# The fake VTR flow: it logs each call to <V>/calls.log and leaves in -temp_dir what VTR would.
FAKE_FLOW = f"""
import shutil, sys
from pathlib import Path
args = sys.argv[1:]
circuit = Path(args[0])
temp = Path(args[args.index("-temp_dir") + 1])
start = args[args.index("-start") + 1] if "-start" in args else None
design = circuit.stem
root = Path(__file__).resolve().parents[2]
with (root / "calls.log").open("a") as log:
    log.write(f"{{design}} {{start or 'full'}}\\n")
fail = root / "fail"  # empty: every design fails; else the designs named
if fail.exists() and (not fail.read_text().split() or design in fail.read_text().split()):
    sys.exit(3)
term = root / "term"  # the designs whose start stops the harness (SIGTERM to its parent)
if term.exists() and design in term.read_text().split():
    import os, signal, time
    os.kill(os.getppid(), signal.SIGTERM)
    time.sleep(30)
hup = root / "hup"  # the designs whose start sends the harness a hangup
if hup.exists() and design in hup.read_text().split():
    import os, signal, time
    os.kill(os.getppid(), signal.SIGHUP)
    time.sleep(0.5)
if (root / "sleep").exists():
    import time
    time.sleep(float((root / "sleep").read_text()))
temp.mkdir(exist_ok=True)
pre = temp / f"{{design}}.pre-vpr.blif"
if start is None:
    pre.write_text({BLIF!r})
    (temp / f"{{design}}.parmys.blif").write_text({BLIF!r})
    (temp / "parmys.out").write_text({TIMEV!r})
else:
    shutil.copyfile(circuit, pre)
if start != "vpr":
    (temp / "abc0.out").write_text("top : lev = 3\\n" + {TIMEV!r})
(temp / "vpr.out").write_text("The entire flow of VPR took 0.25 seconds\\n")
(temp / "vpr.crit_path.out").write_text("The entire flow of VPR took 0.5 seconds\\n")
(temp / "output.txt").write_text("vpr_status=success\\n")
(temp / f"{{design}}.net").write_text("netlist\\n")
print("fake vtr flow done")
"""

FAKE_PARSE = f"""
import sys
fields = {VPR_FIELDS!r}
if "qor_standard" in sys.argv[2]:
    fields = {{"total_runtime": "0.75"}}
print("\\t".join(fields) + "\\t")
print("\\t".join(fields.values()) + "\\t")
"""

FAKE_RANDOM_CHECK = """
import shutil, sys
from pathlib import Path
args = sys.argv[1:]
root = Path(__file__).resolve().parents[2]
with (root / "fcheck-calls.log").open("a") as log:
    log.write(" ".join(args) + "\\n")
# As VTR's run_random_check.py: wipe the work dir first, then need the netlist (yosys fails
# when it is missing).
work = Path(args[args.index("--work-dir") + 1])
if work.exists():
    shutil.rmtree(work)
work.mkdir(parents=True)
if not Path(args[args.index("--post-synth-blif") + 1]).is_file():
    sys.exit(2)
mode = root / "fcheck-exit"
code = int(mode.read_text()) if mode.exists() else 0
print("PASS: 50000 vectors matched across rtl/post-synth\\n"
      "VCHECK_SUMMARY matched=50000 mismatched=0 vectors=50000 port_errors=0" if code == 0 else
      "MISMATCH vec=0 port=y rtl=0 synth=1" if code == 1 else "ERROR: boom")
sys.exit(code)
"""

FAKE_TASK = """
import sys
from pathlib import Path
args = sys.argv[1:]
root = Path(__file__).resolve().parents[2]
run = Path(args[args.index("-temp_dir") + 1]) / "run001"
run.mkdir(parents=True, exist_ok=True)
(run / "parse_results.txt").write_text((root / "task-results").read_text())
"""

FAKE_PATHS = """
import pathlib
root_path = pathlib.Path(__file__).absolute().parent.parent.parent.parent.parent
vpr_exe_path = root_path / "vpr" / "vpr"
abc_exe_path = root_path / "abc" / "abc"
yosys_exe_path = root_path / "build" / "bin" / "yosys"
"""

FAKE_ODIN3 = f"""
import sys
from pathlib import Path
if sys.argv[1:] == ["--help"]:
    print(Path(__file__).with_name("help.txt").read_text())
    sys.exit(0)
if Path(__file__).with_name("fail").exists():
    sys.exit(1)
script = sys.argv[sys.argv.index("-p") + 1]
header = Path(__file__).with_name("abc_line.txt")  # a test's own # abc: line ("" for none)
text = header.read_text() + {BLIF!r} if header.exists() else {ABC_LINE + BLIF!r}
if "--name-style provenance" in script:
    text += "# names: provenance\\n"
Path(script.split()[-1]).write_text(text)
"""

FAKE_TIME = f"""
import subprocess, sys
args = sys.argv[1:]
if args[1] == "-o":
    rc = subprocess.call(args[3:])
    open(args[2], "w").write({TIMEV!r})
else:
    rc = subprocess.call(args[1:])
    sys.stderr.write({TIMEV!r})
sys.exit(rc)
"""


def _script(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"#!{sys.executable}\n{body}")
    path.chmod(0o755)
    return path


def _shell(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"#!/bin/sh\n{body}")
    path.chmod(0o755)
    return path


def _git(repo: Path, *args: str) -> str:
    # gc.auto/maintenance.auto off: a detached auto-gc could still be writing .git when the
    # TemporaryDirectory is removed ("Directory not empty: .../vtr/.git", CI 2026-10-18).
    proc = subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t",
                           "-c", "commit.gpgsign=false", "-c", "gc.auto=0",
                           "-c", "maintenance.auto=false", *args],
                          capture_output=True, text=True, check=True)
    return proc.stdout.strip()


def _tree(path: Any) -> Any:
    """The key tree of a JSON object: nested dicts of key names, leaves None."""
    if isinstance(path, dict):
        return {k: _tree(v) for k, v in path.items()}
    return None


SPEC_TREE = {  # spec §5.2 plus netlist.abc_exchange (point (e))
    "schema": None, "leg": None, "design": None, "arch": None, "key": None, "status": None,
    "gating": None, "vtr_commit": None, "odin3_commit": None, "odin3_build": None,
    "abc_script_sha256": None, "started": None, "elapsed_s": None,
    "netlist": dict.fromkeys(("luts", "ffs", "adders", "mults", "ram_slices", "depth",
                              "abc_depth", "blif_sha256", "abc_exchange")),
    "packed": dict.fromkeys(("num_clb", "num_memories", "num_mult", "device_width",
                             "device_height")),
    "routed": dict.fromkeys(("vpr_status", "min_chan_width", "critical_path_delay_ns",
                             "fmax_mhz", "crit_path_routed_wirelength")),
    "cost": dict.fromkeys(("frontend_wall_s", "frontend_peak_rss_mib", "vpr_time_s",
                           "vpr_total_runtime_s", "max_vpr_mem_mib")),
    "verdicts": dict.fromkeys(("fcheck", "equiv", "rerun")),
    "error": None,
}


class CliTestBase(unittest.TestCase):
    def setUp(self) -> None:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=HEAVY_DIR, prefix="qor-cli-test-",
                                                ignore_cleanup_errors=True)
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.out = self.tmp / "out"
        self.stack = ExitStack()
        self.addCleanup(self.stack.close)
        self.vtr = self.tmp / "vtr"
        self.pin = self._make_vtr()
        self.bin = self.tmp / "bin"
        self.odin3 = _script(self.bin / "odin3", FAKE_ODIN3)
        self.set_help(ALL_PASSES)
        _shell(self.bin / "verilator", 'echo "Verilator 5.020 (fake)"\n')
        self.ldd = _shell(self.tmp / "ldd", 'echo "not a dynamic executable"; exit 1\n')
        time_bin = _script(self.tmp / "time", FAKE_TIME)
        self.repo = self.tmp / "repo"
        (self.repo / "lib" / "abc").mkdir(parents=True)
        (self.repo / "lib" / "vtr.o3lib").write_text("lib\n")
        (self.repo / "lib" / "abc" / "vtr.abc").write_text("strash; if -K 6\n")
        self.golden = self.tmp / "golden"
        self._fake_heavy()
        for module, name, value in (
            (qor_legs, "VTR_PIN", self.pin), (qor_legs, "REPO_ROOT", self.repo),
            (qor_legs, "GOLDEN_ROOT", self.golden), (qor_legs, "TIME_BIN", str(time_bin)),
            (qor_sched, "LDD_CMD", (str(self.ldd),)),
        ):
            self.stack.enter_context(mock.patch.object(module, name, value))
        self.stack.enter_context(mock.patch.dict(
            os.environ, {"PATH": f"{self.bin}:{os.environ.get('PATH', os.defpath)}"}))

    def _make_vtr(self) -> str:
        v = self.vtr
        timing = v / "vtr_flow" / "arch" / "timing"
        timing.mkdir(parents=True)
        for arch in (K6, "EArch"):
            (timing / f"{arch}.xml").write_text(f"<architecture name='{arch}'/>\n")
        verilog = v / "vtr_flow" / "benchmarks" / "verilog"
        verilog.mkdir(parents=True)
        for design in SETS.order:
            (verilog / f"{design}.v").write_text(f"module {design}(); endmodule\n")
        scripts = v / "vtr_flow" / "scripts"
        _script(scripts / "run_vtr_flow.py", FAKE_FLOW)
        _script(scripts / "run_vtr_task.py", FAKE_TASK)
        _script(v / "mosaic" / "verilator_check" / "run_random_check.py", FAKE_RANDOM_CHECK)
        (v / "mosaic" / "verilator_check" / "blif_to_verilog.py").write_text(
            "def pack_bit_blasted_top_ports(text):\n    return text\n")
        libs = scripts / "python_libs" / "vtr"
        _script(libs / "parse_vtr_flow.py", FAKE_PARSE)
        (libs / "__init__.py").write_text("")
        (libs / "paths.py").write_text(FAKE_PATHS)
        (libs / "log_parse.py").write_text("def determine_lut_size(arch):\n    return 6\n")
        for config in ("parse_config/vpr_standard.txt", "qor_config/qor_standard.txt"):
            (v / "vtr_flow" / "parse" / config).parent.mkdir(parents=True, exist_ok=True)
            (v / "vtr_flow" / "parse" / config).write_text("# fake\n")
        _shell(v / "vpr" / "vpr", "exit 0\n")
        _shell(v / "abc" / "abc", "exit 0\n")
        _shell(v / "build" / "bin" / "yosys", 'echo "Yosys 0.55 (git sha1 fake)"\n')
        (v / "README").write_text("fake VTR\n")
        _git(v, "init", "-q")
        _git(v, "add", "-A")
        _git(v, "commit", "-q", "-m", "pin")
        python = v / ".venv" / "bin" / "python"  # untracked, as VTR's own venv
        python.parent.mkdir(parents=True)
        python.symlink_to(sys.executable)
        return _git(v, "rev-parse", "HEAD")

    def _fake_heavy(self) -> None:
        root = self.tmp / "cg"
        heavy_slice = root / SLICE_DIRS / "odin3-heavy.slice"
        leaf = heavy_slice / "odin3-heavy-4242-1760000000000000000.scope"
        leaf.mkdir(parents=True)
        for directory, mem, swap in ((leaf, "max", "max"), (heavy_slice, str(6 * GIB), "0")):
            (directory / "memory.max").write_text(mem + "\n")
            (directory / "memory.swap.max").write_text(swap + "\n")
        (heavy_slice / "memory.events").write_text("oom 0\noom_kill 0\n")
        self_cgroup = self.tmp / "self-cgroup"
        self_cgroup.write_text(f"0::/{leaf.relative_to(root)}\n")
        self.lock = self.tmp / "odin3-heavy.lock"
        self.lock.touch()
        fd9 = self.tmp / "fd9"
        fd9.symlink_to(self.lock)
        self.df_file = self.tmp / "df-gib"
        self.df_file.write_text("100\n")
        fake_df = self.tmp / "fake-df.py"
        fake_df.write_text("import sys\ngib = int(open(sys.argv[1]).read())\n"
                           f"print('Mounted on   Avail')\nprint('/mnt/c', gib * {GIB})\n")
        self.meminfo = self.tmp / "meminfo"
        self.meminfo.write_text(f"MemTotal: {24 * KIB_PER_GIB} kB\n"
                                f"MemAvailable: {16 * KIB_PER_GIB} kB\n")
        self.heavy_run = _shell(self.tmp / "heavy-run",
                                'systemd-run --user --scope -p OOMPolicy=continue "$@"\n')
        for module, name, value in (
            (qor_heavy, "SELF_CGROUP", self_cgroup), (qor_heavy, "CGROUP_ROOT", root),
            (qor_heavy, "FD9", fd9), (qor_heavy, "HEAVY_LOCK", self.lock),
            (qor_heavy, "HEAVY_RUN", self.heavy_run),
            (qor_heavy, "DF_CMD", (sys.executable, "-I", str(fake_df), str(self.df_file))),
            (vtr_qor, "MEMINFO", self.meminfo), (vtr_qor, "SYSTEMD_PROBE", ("true",)),
        ):
            self.stack.enter_context(mock.patch.object(module, name, value))
        self.stack.enter_context(mock.patch.dict(os.environ,
                                                 {"ODIN3_HEAVY_SLICE": "odin3-heavy.slice"}))

    # -- helpers ---------------------------------------------------------------------------

    def set_help(self, passes: tuple[str, ...]) -> None:
        lines = ["usage: odin3 [options] -p <script>", "", "options:", "  --help  this text",
                 "", "passes:"] + [f"  {name}  the {name} pass" for name in passes]
        (self.bin / "help.txt").write_text("\n".join(lines) + "\n")

    def cli(self, *argv: str, common: bool = True) -> tuple[int, str, str]:
        extra = ["--vtr-root", str(self.vtr), "--odin3", str(self.odin3),
                 "--out", str(self.out)] if common else []
        return run_main(vtr_qor.main, [*argv, *extra])

    def calls(self) -> list[str]:
        log = self.vtr / "calls.log"
        return log.read_text().splitlines() if log.exists() else []

    def harness_lines(self) -> list[str]:
        log = self.out / "harness.log"
        return log.read_text().splitlines() if log.exists() else []

    def job_dirs(self, leg: str, design: str) -> list[Path]:
        return sorted((self.out / self.pin[:12] / K6 / leg).glob(f"*/{design}"))

    def load(self, path: Path) -> dict[str, Any]:
        data = json.loads(path.read_text())
        assert isinstance(data, dict)
        return data


class DryRunTest(CliTestBase):
    def test_dry_run_deterministic(self) -> None:
        """T7 (part): five lines in §2 order, identical twice, nothing run or written."""
        first = self.cli("run", "--leg", "P", "--set", "smoke", "--dry-run")
        second = self.cli("run", "--leg", "P", "--set", "smoke", "--dry-run")
        self.assertEqual(first[0], 0, first[2])
        self.assertEqual(first, second)
        lines = first[1].splitlines()
        self.assertEqual(len(lines), 5)
        self.assertEqual([line.split()[1] for line in lines], list(SETS.smoke))
        for line in lines:
            self.assertRegex(line, r"^P \w+ [0-9a-f]{12} run$")
        self.assertEqual(len({line.split()[2] for line in lines}), 1)  # one leg key
        self.assertEqual(self.calls(), [])
        self.assertFalse(self.out.exists())

    def test_dry_run_order_and_cached(self) -> None:
        """Designs given out of order keep §2's (group B last); a complete job shows cached."""
        code, out, err = self.cli("run", "--leg", "P", "--designs",
                                  "LU8PEEng,sha,ch_intrinsics", "--dry-run")
        self.assertEqual(code, 0, err)
        self.assertEqual([line.split()[1] for line in out.splitlines()],
                         ["ch_intrinsics", "sha", "LU8PEEng"])
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        code, out, _ = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics,sha",
                                "--dry-run")
        self.assertEqual([line.split()[3] for line in out.splitlines()], ["cached", "run"])
        (job,) = self.job_dirs("P", "ch_intrinsics")
        (job / "status").unlink()  # half done: metrics.json ok, no status
        code, out, _ = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics", "--dry-run")
        self.assertEqual(out.split()[3], "run")

    def test_isolated_import(self) -> None:
        """Point (u): ``python3 -I vtr_qor.py --help`` works; the shim prints the same."""
        direct = subprocess.run([sys.executable, "-I", str(CLI), "--help"],
                                capture_output=True, text=True, check=False)
        self.assertEqual(direct.returncode, 0, direct.stderr)
        self.assertIn("usage: vtr-qor", direct.stdout)
        shim = subprocess.run([str(SHIM), "--help"], capture_output=True, text=True, check=False)
        self.assertEqual(shim.returncode, 0, shim.stderr)
        self.assertEqual(shim.stdout, direct.stdout)


class InputTest(CliTestBase):
    def test_vtr_errors(self) -> None:
        missing = self.tmp / "nowhere"
        code, _, err = run_main(vtr_qor.main, ["run", "--leg", "P", "--set", "smoke",
                                               "--vtr-root", str(missing),
                                               "--out", str(self.out)])
        self.assertEqual(code, 2)
        self.assertEqual(err, f"vtr-qor: VTR not found at {missing} (set VTR_ROOT or "
                              "--vtr-root)\n")
        # Another commit.
        (self.vtr / "README").write_text("moved on\n")
        _git(self.vtr, "commit", "-q", "-am", "next")
        other = _git(self.vtr, "rev-parse", "HEAD")
        code, _, err = self.cli("run", "--leg", "P", "--set", "smoke")
        self.assertEqual(code, 2)
        self.assertEqual(err, f"vtr-qor: VTR at {self.vtr} is {other[:12]}, ORACLES.md pins "
                              f"{self.pin[:12]} (use --allow-vtr-drift for a report-only run)\n")
        # Back at the pin with one modified tracked file (an untracked one does not count).
        _git(self.vtr, "checkout", "-q", self.pin)
        (self.vtr / "README").write_text("edited\n")
        (self.vtr / "untracked.txt").write_text("x\n")
        code, _, err = self.cli("run", "--leg", "P", "--set", "smoke")
        self.assertEqual(code, 2)
        self.assertEqual(err, f"vtr-qor: VTR at {self.vtr} has 1 modified tracked files\n")

    def test_drift_flag_never_waives_a_modified_tree(self) -> None:
        """Ruling I1 (the reviewer's reproduction): --allow-vtr-drift on a modified tree at the
        pin is refused, so nothing is cached under the clean key and latest does not move; the
        clean run afterwards starts VTR."""
        (self.vtr / "README").write_text("patched VPR, say\n")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics",
                                "--allow-vtr-drift")
        self.assertEqual(code, 2)
        self.assertEqual(err, f"vtr-qor: VTR at {self.vtr} has 1 modified tracked files\n")
        self.assertEqual(self.calls(), [])
        self.assertFalse((self.out / self.pin[:12]).exists())
        _git(self.vtr, "checkout", "-q", "--", "README")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.calls(), ["ch_intrinsics full"])

    def test_drift_run_is_report_only(self) -> None:
        """VTR at another commit with --allow-vtr-drift: its own vtr12, job.json says drift,
        and latest never moves."""
        (self.vtr / "README").write_text("moved on\n")
        _git(self.vtr, "commit", "-q", "-am", "next")
        other = _git(self.vtr, "rev-parse", "HEAD")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics",
                                "--allow-vtr-drift")
        self.assertEqual(code, 0, err)
        (job,) = sorted((self.out / other[:12] / K6 / "P").glob("*/ch_intrinsics"))
        self.assertIs(self.load(job / "job.json")["vtr_drift"], True)
        self.assertEqual((job / "status").read_text(), "ok\n")
        self.assertFalse((job.parents[1] / "latest").exists())
        self.assertFalse((self.out / self.pin[:12]).exists())

    def test_clean_vtr_not_drift(self) -> None:
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        self.assertIs(self.load(job / "job.json")["vtr_drift"], False)
        self.assertEqual(vtr_qor.check_vtr(self.vtr, False), self.pin)

    def test_unknown_inputs(self) -> None:
        code, _, err = self.cli("run", "--leg", "P", "--designs", "nope")
        self.assertEqual(code, 2)
        self.assertEqual(err, "vtr-qor: unknown design 'nope' (see tests/vtr/qor/sets.json)\n")
        code, _, err = self.cli("run", "--leg", "P", "--set", "big")
        self.assertEqual(code, 2)
        self.assertEqual(err, "vtr-qor: unknown set 'big' (gate, extra, smoke)\n")
        code, _, err = self.cli("run", "--leg", "Q", "--set", "smoke")
        self.assertEqual(code, 2)
        self.assertEqual(err, "vtr-qor: unknown leg 'Q' (P, O, Oprime, Pprime, R, Oprov)\n")
        code, _, err = self.cli("run", "--leg", "P")
        self.assertEqual((code, err), (2, "vtr-qor: run needs --set or --designs\n"))

    def test_leg_unavailable(self) -> None:
        """E-QH5, with the leg's first missing pass."""
        self.set_help(tuple(p for p in ALL_PASSES if p != "wreduce"))
        code, _, err = self.cli("run", "--leg", "O", "--set", "smoke", "--dry-run")
        self.assertEqual(code, 2)
        self.assertEqual(err, f"vtr-qor: leg O needs pass 'wreduce', which {self.odin3} does "
                              "not register\n")
        code, _, err = self.cli("run", "--leg", "P'", "--set", "smoke", "--dry-run")
        self.assertEqual(code, 0, err)  # Pprime needs no wreduce; P' is its alias
        code, _, err = self.cli("run", "--leg", "P", "--set", "smoke", "--dry-run",
                                "--odin3", str(self.tmp / "absent"))
        self.assertEqual(code, 0, err)  # P runs no odin3

    def test_options_refused(self) -> None:
        code, _, err = self.cli("run", "--leg", "P", "--set", "smoke", "-j", "2", "--dry-run")
        self.assertEqual((code, err), (2, "vtr-qor: -j 2 exceeds 1 (the heavy-run rule)\n"))
        code, _, err = self.cli("run", "--leg", "P", "--set", "smoke", "--xl", "--dry-run")
        self.assertEqual(code, 2)
        self.assertIn("--xl is only for run --designs", err)
        code, _, err = run_main(vtr_qor.main, ["run", "--leg", "P", "--set", "smoke",
                                               "--dry-run", "--out", "/etc"])
        self.assertEqual(code, 2)
        self.assertEqual(err, "vtr-qor: refusing --out /etc: not under ~/odin3-ws/work or "
                              "/tmp/claude-1000\n")

    def test_run_outside_heavy_refused(self) -> None:
        """No fake heavy run (no slice variable) and no ODIN3_HEAVY_OK: refused, nothing run."""
        with mock.patch.dict(os.environ, {"ODIN3_HEAVY_OK": "", "ODIN3_HEAVY_SLICE": ""}):
            code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
        self.assertEqual(code, 2)
        self.assertEqual(err, "vtr-qor: heavy runs are the controller's: set ODIN3_HEAVY_OK=1\n")
        self.assertEqual(self.calls(), [])


class RunTest(CliTestBase):
    def test_run_reuses_cache(self) -> None:
        """Review Focus 2: a second run starts no job; --rerun does; a new odin3 build re-runs
        only the odin3 legs."""
        args = ("run", "--leg", "P,O", "--designs", "ch_intrinsics")
        code, _, err = self.cli(*args)
        self.assertEqual(code, 0, err)
        self.assertEqual(self.calls(), ["ch_intrinsics full", "ch_intrinsics vpr"])
        ran = re.compile(r"\] (P|O) ch_intrinsics (ok|failed|capped|timeout) ")
        self.assertEqual(sum(bool(ran.search(line)) for line in self.harness_lines()), 2)

        before = len(self.harness_lines())
        self.assertEqual(self.cli(*args)[0], 0)
        self.assertEqual(len(self.calls()), 2)
        new = self.harness_lines()[before:]
        self.assertFalse(any(ran.search(line) for line in new), new)
        self.assertFalse(any(line.endswith(" start") for line in new), new)
        self.assertEqual(sum(line.endswith(" cached") for line in new), 2)

        self.assertEqual(self.cli(*args, "--rerun")[0], 0)
        self.assertEqual(len(self.calls()), 4)

        with self.odin3.open("a") as handle:
            handle.write("# rebuilt\n")
        before = len(self.harness_lines())
        self.assertEqual(self.cli(*args)[0], 0)
        self.assertEqual(self.calls()[4:], ["ch_intrinsics vpr"])
        new = self.harness_lines()[before:]
        self.assertTrue(any(re.search(r"\] O ch_intrinsics ok ", line) for line in new), new)
        self.assertTrue(any(line.endswith("] P ch_intrinsics cached") for line in new), new)

    def test_metrics_schema(self) -> None:
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics,spree")
        self.assertEqual(code, 0, err)
        for design, gating in (("ch_intrinsics", True), ("spree", False)):
            (job,) = self.job_dirs("P", design)
            metrics = self.load(job / "metrics.json")
            self.assertEqual(_tree(metrics), SPEC_TREE)
            self.assertEqual(metrics["schema"], 1)
            self.assertIs(metrics["gating"], gating)
            self.assertEqual((metrics["status"], metrics["design"], metrics["arch"]),
                             ("ok", design, K6))
            self.assertEqual(metrics["vtr_commit"], self.pin)
            self.assertIsNone(metrics["odin3_build"])
            self.assertRegex(metrics["started"], r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$")
            self.assertEqual(metrics["netlist"], {
                "luts": 1, "ffs": 1, "adders": 0, "mults": 0, "ram_slices": 0, "depth": 1,
                "abc_depth": 3, "abc_exchange": None,
                "blif_sha256": hashlib.sha256(BLIF.encode()).hexdigest()})
            self.assertEqual(metrics["packed"], {"num_clb": 12, "num_memories": 1, "num_mult": 2,
                                                 "device_width": 8, "device_height": 9})
            self.assertEqual(metrics["routed"], {
                "vpr_status": "success", "min_chan_width": 40, "critical_path_delay_ns": 2.5,
                "fmax_mhz": 400.0, "crit_path_routed_wirelength": 1234})
            self.assertEqual(metrics["cost"], {
                "frontend_wall_s": 1.0, "frontend_peak_rss_mib": 20, "vpr_time_s": 0.75,
                "vpr_total_runtime_s": 0.75, "max_vpr_mem_mib": 64})
            self.assertEqual(metrics["verdicts"],
                             {"fcheck": "skipped", "equiv": "skipped", "rerun": "skipped"})
            self.assertEqual(metrics["key"], self._planned_key(design))
            # Cleaned up (spec §7.5): the keep list plus the gzipped pre-VPR netlist.
            self.assertEqual(sorted(p.name for p in (job / "run").iterdir()), sorted(
                ["abc0.out", "output.txt", "parmys.out", "parse_results.txt", "qor_results.txt",
                 "parse_results.err", "qor_results.err",
                 "vpr.crit_path.out", "vpr.out", "vtr_flow.out", f"{design}.pre-vpr.blif.gz"]))

    def _planned_key(self, design: str) -> str:
        req = qor_sched.PlanRequest(legs=("P",), designs=(design,), arch="k6", out=self.out,
                                    vtr_root=self.vtr, odin3=self.odin3, rerun=False)
        (job,) = qor_sched.plan_jobs(req, SETS)
        return job.key

    def test_job_json(self) -> None:
        golden = self.golden / K6 / "vtr" / "ch_intrinsics" / "ch_intrinsics.parmys.blif"
        golden.parent.mkdir(parents=True)
        golden.write_text(BLIF)
        with mock.patch.dict(os.environ, {"ODIN3_QOR_MARK": "m", "UNRELATED": "u"}):
            code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        (job_dir,) = self.job_dirs("P", "ch_intrinsics")
        record = self.load(job_dir / "job.json")
        self.assertEqual((record["leg"], record["design"], record["key12"]),
                         ("P", "ch_intrinsics", job_dir.parent.name))
        inputs = record["key_inputs"]
        self.assertEqual(inputs["vtr_commit"], self.pin)
        self.assertEqual(qor_sched.job_key(qor_sched.KeyInputs(**{
            **inputs, "params": tuple(inputs["params"])})), record["key"])
        argvs = [c["argv"] for c in record["commands"]]
        self.assertEqual(len(argvs), 3)  # VTR, then the two parse commands
        self.assertTrue(argvs[0][1].endswith("run_vtr_flow.py"))
        self.assertTrue(all(a[1].endswith("parse_vtr_flow.py") for a in argvs[1:]))
        self.assertEqual(record["env"]["ODIN3_QOR_MARK"], "m")
        self.assertNotIn("UNRELATED", record["env"])
        self.assertIn("PATH", record["env"])
        self.assertEqual(record["slice"], "odin3-heavy.slice")
        self.assertEqual(record["host"]["mem_gib"], 24.0)
        self.assertRegex(record["ended"], r"Z$")
        # Point (l): VTR's parmys.blif against the golden, before cleanup removed it.
        self.assertEqual(record["parmys_blif_sha256"], hashlib.sha256(BLIF.encode()).hexdigest())
        self.assertIs(record["parmys_equals_golden"], True)
        self.assertFalse((job_dir / "run" / "ch_intrinsics.parmys.blif").exists())
        golden.write_text(BLIF + "# other\n")
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics,spree",
                                  "--rerun")[0], 0)
        self.assertIs(self.load(job_dir / "job.json")["parmys_equals_golden"], False)
        (spree,) = self.job_dirs("P", "spree")
        self.assertIsNone(self.load(spree / "job.json")["parmys_equals_golden"])  # no golden

    def test_odin3_leg_metrics(self) -> None:
        """O: abc_depth and the exchange from the # abc: header (point (e)); the front-end cost
        from frontend.time; the odin3 build in metrics.json."""
        code, _, err = self.cli("run", "--leg", "O", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        (job,) = self.job_dirs("O", "ch_intrinsics")
        metrics = self.load(job / "metrics.json")
        self.assertEqual(_tree(metrics), SPEC_TREE)
        self.assertEqual((metrics["netlist"]["abc_depth"], metrics["netlist"]["abc_exchange"]),
                         (2, "aig"))
        self.assertEqual((metrics["cost"]["frontend_wall_s"],
                          metrics["cost"]["frontend_peak_rss_mib"]), (0.5, 20))
        self.assertRegex(metrics["odin3_build"], r"^[0-9a-f]{64}$")
        self.assertTrue((job / "ch_intrinsics.blif.sha256").is_file())

    def test_failed_job_keeps_job_json(self) -> None:
        """A VTR failure: no metrics.json, exit 1, job.json from the prepare hook."""
        (self.vtr / "fail").write_text("")  # untracked: the tree stays clean
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
        self.assertEqual(code, 1, err)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        self.assertEqual((job / "status").read_text(), "failed\n")
        self.assertFalse((job / "metrics.json").exists())
        self.assertEqual(self.load(job / "job.json")["ended"],  # M7: the status file's time
                         vtr_qor._utc((job / "status").stat().st_mtime))

    def latest(self, leg: str) -> Path:
        return self.out / self.pin[:12] / K6 / leg / "latest"

    def test_latest_own_planned_set(self) -> None:
        """Point (f) as ruled: a leg key is complete when its own planned set (planned.json, the
        union of every run under it) is; latest is logged before done:, and status reports
        it."""
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        key12 = job.parent.name
        self.assertEqual(self.latest("P").read_text(), f"{key12}\n")
        self.assertEqual(self.load(job.parent / "planned.json"), {"designs": ["ch_intrinsics"]})
        lines = self.harness_lines()
        at = lines.index(f"vtr-qor: latest: P {key12}")
        self.assertTrue(lines[at + 1].startswith("vtr-qor: done: 1 ok"), lines)
        # A second design joins the planned set; latest stays (still complete).
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "spree")[0], 0)
        self.assertEqual(self.load(job.parent / "planned.json"),
                         {"designs": ["ch_intrinsics", "spree"]})
        code, out, _ = self.cli("status")
        self.assertEqual((code, out), (0, f"{K6} P latest {key12} ok 2 failed 0 age 0d\n"))

    def test_latest_retracted(self) -> None:
        """Review M2: a failed rerun of the latest key leaves no latest (retracted as the
        attempt starts); and update_latest itself retracts a latest whose key lost a result
        some other way (here: metrics.json deleted by hand)."""
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        self.assertTrue(self.latest("P").exists())
        (self.vtr / "fail").write_text("")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics", "--rerun")
        self.assertEqual(code, 1, err)
        self.assertFalse(self.latest("P").exists())
        self.assertIn("retracted (its ch_intrinsics job reruns)", err)
        (self.vtr / "fail").unlink()
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        key12 = job.parent.name
        self.assertEqual(self.latest("P").read_text(), f"{key12}\n")
        (job / "metrics.json").unlink()
        req = qor_sched.PlanRequest(legs=("P",), designs=("ch_intrinsics",), arch="k6",
                                    out=self.out, vtr_root=self.vtr, odin3=self.odin3,
                                    rerun=False)
        self.assertEqual(vtr_qor.update_latest(req, SETS, drift=False),
                         [f"latest: P {key12} retracted (no longer complete)"])
        self.assertFalse(self.latest("P").exists())

    def test_latest_never_on_a_growing_key(self) -> None:
        """Review I3 (the reviewer's reproduction): a run that adds spree to the latest key and
        is stopped (SIGTERM, exit 143) when spree starts leaves no latest on the now incomplete
        key; status says so. The run that completes the key sets latest again."""
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        key12 = job.parent.name
        self.assertTrue(self.latest("P").exists())
        (self.vtr / "term").write_text("spree\n")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics,spree")
        self.assertEqual(code, 143, err)
        self.assertEqual(self.load(job.parent / "planned.json"),
                         {"designs": ["ch_intrinsics", "spree"]})
        self.assertFalse(self.latest("P").exists())
        self.assertIn(f"vtr-qor: latest: P {key12} retracted (its planned set grows)",
                      self.harness_lines())
        code, out, _ = self.cli("status")
        self.assertEqual(out, f"{K6} P latest - ok 1 failed 0 age 0d\n")
        (self.vtr / "term").unlink()
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "spree")[0], 0)
        self.assertEqual(self.latest("P").read_text(), f"{key12}\n")

    def test_hangup_under_nohup(self) -> None:
        """Final review I1 (the reproduction): a SIGHUP mid-job ends the run (exit 129) unless
        SIGHUP was inherited as ignored, as nohup leaves it; then the run finishes ok and the
        disposition is still SIG_IGN afterwards."""
        (self.vtr / "hup").write_text("ch_intrinsics\n")
        previous = signal.signal(signal.SIGHUP, signal.SIG_IGN)
        try:
            code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
            self.assertEqual(code, 0, err)
            self.assertIs(signal.getsignal(signal.SIGHUP), signal.SIG_IGN)
            signal.signal(signal.SIGHUP, lambda signum, frame: None)  # not ignored
            code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics",
                                    "--rerun")
            self.assertEqual(code, 129, err)
        finally:
            signal.signal(signal.SIGHUP, previous)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        self.assertFalse((job / "metrics.json").exists())

    def test_killed_rerun_retracts_latest(self) -> None:
        """Review I3, addendum 2 (the reproduction): a --rerun of the latest key stopped by
        SIGTERM mid-attempt leaves no latest on the key whose results it wiped; a completed
        rerun sets latest again."""
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        key12 = job.parent.name
        (self.vtr / "term").write_text("ch_intrinsics\n")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics", "--rerun")
        self.assertEqual(code, 143, err)
        self.assertFalse((job / "metrics.json").exists())
        self.assertFalse(self.latest("P").exists())
        self.assertIn(f"vtr-qor: latest: P {key12} retracted (its ch_intrinsics job reruns)",
                      self.harness_lines())
        code, out, _ = self.cli("status")
        self.assertEqual(out, f"{K6} P latest - ok 0 failed 0 age -\n")  # wiped: no status
        (self.vtr / "term").unlink()
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics",
                                  "--rerun")[0], 0)
        self.assertEqual(self.latest("P").read_text(), f"{key12}\n")

    def test_timed_out_rerun_retracts_latest(self) -> None:
        """A --rerun of the latest key that times out (exit 3): no latest on the wiped key."""
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        (self.vtr / "sleep").write_text("30")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics", "--rerun",
                                "--job-timeout", "1")
        self.assertEqual(code, 3, err)
        self.assertFalse(self.latest("P").exists())

    def test_clean_keeps_latest_results(self) -> None:
        """clean --keep 1 with latest on the older of two complete keys: latest's key and its
        results stay whole."""
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        newer = job.parents[1] / "ffffffffffff" / "ch_intrinsics"
        (newer / "run").mkdir(parents=True)
        (newer / "status").write_text("ok\n")
        (newer / "metrics.json").write_text(json.dumps({"status": "ok"}))
        (newer.parent / "planned.json").write_text(json.dumps({"designs": ["ch_intrinsics"]}))
        before = sorted(str(p.relative_to(job)) for p in job.rglob("*"))
        code, _, err = self.cli("clean", "--keep", "1")
        self.assertEqual(code, 0, err)
        self.assertEqual(sorted(str(p.relative_to(job)) for p in job.rglob("*")), before)
        self.assertEqual(self.latest("P").read_text(), f"{job.parent.name}\n")
        self.assertTrue(newer.is_dir())

    def test_latest_judged_by_planned_set(self) -> None:
        """Review N1: a key whose planned spree failed stays without latest even when a later
        run of only its complete design (cached) ends ok."""
        (self.vtr / "fail").write_text("spree\n")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics,spree")
        self.assertEqual(code, 1, err)
        self.assertFalse(self.latest("P").exists())
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        self.assertIn("] P ch_intrinsics cached", err)
        self.assertFalse(self.latest("P").exists())

    def test_no_planned_set_never_complete(self) -> None:
        """Review N2: a key without planned.json is never complete, so never latest."""
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        self.latest("P").unlink()
        (job.parent / "planned.json").unlink()
        req = qor_sched.PlanRequest(legs=("P",), designs=("ch_intrinsics",), arch="k6",
                                    out=self.out, vtr_root=self.vtr, odin3=self.odin3,
                                    rerun=False)
        self.assertEqual(vtr_qor.update_latest(req, SETS, drift=False), [])
        self.assertFalse(self.latest("P").exists())

    def test_latest_per_leg(self) -> None:
        """--leg P,O with O's front end failing: only P's latest."""
        (self.bin / "fail").write_text("")
        code, _, err = self.cli("run", "--leg", "P,O", "--designs", "ch_intrinsics")
        self.assertEqual(code, 1, err)
        self.assertTrue(self.latest("P").exists())
        self.assertFalse(self.latest("O").exists())
        self.assertTrue((self.latest("P").parent).is_dir())

    def test_stage_and_job_timeout(self) -> None:
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics",
                                "--stage-timeout", "77", "--job-timeout", "99")
        self.assertEqual(code, 0, err)
        (job,) = self.job_dirs("P", "ch_intrinsics")
        argv = self.load(job / "job.json")["commands"][0]["argv"]
        self.assertEqual(argv[argv.index("-timeout") + 1], "77")

    def test_job_timeout(self) -> None:
        """--job-timeout reaches the runner: a 1 s limit on a 300 s VTR is a timeout (exit 3)."""
        (self.vtr / "sleep").write_text("300")
        start = time.monotonic()
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics",
                                "--job-timeout", "1")
        self.assertEqual(code, 3, err)
        self.assertLess(time.monotonic() - start, 120)  # far below the fake VTR's 300 s
        self.assertRegex(err, r"vtr-qor: P ch_intrinsics: timeout after 1 s in \S+ "
                              r"\(elapsed \d+ s\)")
        (job,) = self.job_dirs("P", "ch_intrinsics")
        self.assertEqual((job / "status").read_text(), "timeout\n")

    def test_job_timeout_zero_is_accepted(self) -> None:
        """--job-timeout 0 means no cap; a negative value is a usage error."""
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics",
                                "--job-timeout", "0")
        self.assertEqual(code, 0, err)
        code, _, _ = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics",
                              "--job-timeout", "-1")
        self.assertEqual(code, 2)

    def test_keep_netlists(self) -> None:
        """--keep-netlists keeps O's <J>/<d>.blif and pre-VPR netlist (point (n))."""
        code, _, err = self.cli("run", "--leg", "O", "--designs", "ch_intrinsics",
                                "--keep-netlists")
        self.assertEqual(code, 0, err)
        (job,) = self.job_dirs("O", "ch_intrinsics")
        self.assertTrue((job / "ch_intrinsics.blif").is_file())
        self.assertTrue((job / "run" / "ch_intrinsics.pre-vpr.blif").is_file())
        self.assertFalse((job / "ch_intrinsics.blif.sha256").exists())

    def test_retention_after_run(self) -> None:
        """Retention runs after a run, by each key's own planned set: with three older complete
        keys, the run's own (fourth) key prunes the oldest; latest is protected."""
        leg = self.out / self.pin[:12] / K6 / "P"
        now = time.time()
        for i, key in enumerate(("aaaaaaaaaaaa", "bbbbbbbbbbbb", "cccccccccccc")):
            job = leg / key / "ch_intrinsics"
            (job / "run").mkdir(parents=True)
            (job / "status").write_text("ok\n")
            (job / "metrics.json").write_text(json.dumps({"status": "ok"}))
            (leg / key / "planned.json").write_text(json.dumps({"designs": ["ch_intrinsics"]}))
            os.utime(job / "status", (now - 1000 + i, now - 1000 + i))
        incomplete = leg / "dddddddddddd" / "sha"  # no planned.json: never complete
        incomplete.mkdir(parents=True)
        os.utime(incomplete, (now - 5000, now - 5000))
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        (job,) = [j for j in self.job_dirs("P", "ch_intrinsics")
                  if j.parent.name not in ("bbbbbbbbbbbb", "cccccccccccc")]
        self.assertEqual(sorted(p.name for p in leg.iterdir() if p.is_dir()),
                         sorted(["bbbbbbbbbbbb", "cccccccccccc", job.parent.name]))

    def test_oprime_metrics(self) -> None:
        """O': VTR's abc_depth (no exchange), and the front-end cost adds VTR's ABC stage to
        frontend.time (0.5 + 0.5 s)."""
        code, _, err = self.cli("run", "--leg", "Oprime", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        (job,) = self.job_dirs("Oprime", "ch_intrinsics")
        metrics = self.load(job / "metrics.json")
        self.assertEqual((metrics["netlist"]["abc_depth"], metrics["netlist"]["abc_exchange"]),
                         (3, None))
        self.assertEqual((metrics["cost"]["frontend_wall_s"],
                          metrics["cost"]["frontend_peak_rss_mib"]), (1.0, 20))
        self.assertEqual(self.calls(), ["ch_intrinsics abc"])

    def test_odin3_commit(self) -> None:
        """Review M5: odin3_commit only for an odin3 built in this repo (with -dirty), never
        for P."""
        _git(self.repo, "init", "-q")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", "repo")
        head = _git(self.repo, "rev-parse", "HEAD")
        inside = self.repo / "build" / "odin3"
        inside.parent.mkdir()
        inside.write_bytes(self.odin3.read_bytes())
        inside.chmod(0o755)
        (inside.parent / "help.txt").write_text((self.bin / "help.txt").read_text())
        code, _, err = run_main(vtr_qor.main, [
            "run", "--leg", "P,O", "--designs", "ch_intrinsics", "--vtr-root", str(self.vtr),
            "--odin3", str(inside), "--out", str(self.out)])
        self.assertEqual(code, 0, err)
        (p_job,) = self.job_dirs("P", "ch_intrinsics")
        (o_job,) = self.job_dirs("O", "ch_intrinsics")
        self.assertIsNone(self.load(p_job / "metrics.json")["odin3_commit"])
        self.assertEqual(self.load(o_job / "metrics.json")["odin3_commit"], head)
        (self.repo / "lib" / "vtr.o3lib").write_text("edited\n")
        self.assertEqual(vtr_qor.odin3_commit(("O",), inside), f"{head}-dirty")
        self.assertIsNone(vtr_qor.odin3_commit(("O",), self.odin3))  # built elsewhere
        self.assertIsNone(vtr_qor.odin3_commit(("P",), inside))


class ControlLegTest(CliTestBase):
    """Plan Task 10: P' and R read the verified Parmys golden; point (e) and (o)."""

    def golden_for(self, design: str, **prov: str) -> Path:
        return write_golden(self.golden / K6, design, BLIF, **prov)

    def odin3_commands(self, job_dir: Path) -> list[list[str]]:
        record = self.load(job_dir / "job.json")
        return [c["argv"] for c in record["commands"]]

    def test_pprime_skipped(self) -> None:
        """A bad golden: status skipped, metrics.json says why (E-QH6), exit 1, nothing runs."""
        golden = self.golden_for("ch_intrinsics", status="failed")
        code, _, err = self.cli("run", "--leg", "P'", "--designs", "ch_intrinsics")
        text = f"ch_intrinsics: Parmys golden {golden}: status=failed"
        self.assertEqual(code, 1, err)
        self.assertIn(f"vtr-qor: {text}\n", err)
        (job,) = self.job_dirs("Pprime", "ch_intrinsics")
        self.assertEqual((job / "status").read_text(), "skipped\n")
        metrics = self.load(job / "metrics.json")
        self.assertEqual((metrics["status"], metrics["error"]), ("skipped", text))
        self.assertEqual(self.calls(), [])
        self.assertIn("1 skipped", self.harness_lines()[-1])
        # No golden at all is `missing`; the same for R; a good golden runs.
        code, _, err = self.cli("run", "--leg", "R", "--designs", "stereovision3")
        self.assertEqual(code, 1, err)
        missing = qor_legs.golden_blif(K6, "stereovision3")
        self.assertIn(f"stereovision3: Parmys golden {missing}: missing", err)
        code, out, _ = self.cli("run", "--leg", "P',R", "--designs", "ch_intrinsics",
                                "--dry-run")
        self.assertEqual((code, [line.split()[-1] for line in out.splitlines()]),
                         (0, ["skipped", "skipped"]))
        self.golden_for("ch_intrinsics")
        code, _, err = self.cli("run", "--leg", "P'", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.calls(), ["ch_intrinsics vpr"])

    def test_pprime_commands(self) -> None:
        """P' reads the golden, abc runs in odin3, VTR starts at vpr; R starts at abc and its
        script has no abc; both key on the golden's SHA-256."""
        golden = self.golden_for("ch_intrinsics")
        code, _, err = self.cli("run", "--leg", "P',R", "--designs", "ch_intrinsics",
                                "--keep-netlists")
        self.assertEqual(code, 0, err)
        self.assertEqual(sorted(self.calls()), ["ch_intrinsics abc", "ch_intrinsics vpr"])
        (pj,) = self.job_dirs("Pprime", "ch_intrinsics")
        (rj,) = self.job_dirs("R", "ch_intrinsics")
        p_front, p_vtr, *_ = self.odin3_commands(pj)
        r_front, r_vtr, *_ = self.odin3_commands(rj)
        self.assertIn(f"read_blif {golden}; abc --lut 6; write_blif --vtr --lut-k 6",
                      p_front[-1])
        self.assertIn(f"read_blif {golden}; write_blif --vtr --lut-k 6", r_front[-1])
        self.assertNotIn("abc --lut", r_front[-1])
        self.assertEqual((p_vtr[p_vtr.index("-start") + 1], r_vtr[r_vtr.index("-start") + 1]),
                         ("vpr", "abc"))
        sha = hashlib.sha256(golden.read_bytes()).hexdigest()
        for job in (pj, rj):
            self.assertEqual(self.load(job / "job.json")["key_inputs"]["source_sha256"], sha)

    def test_abc_header_metrics(self) -> None:
        """P': the # abc: line gives abc_depth and the exchange; without it both are null."""
        self.golden_for("ch_intrinsics")
        line = ABC_LINE.replace("exchange=aig depth=2", "exchange=blif depth=5")
        (self.bin / "abc_line.txt").write_text(line)
        code, _, err = self.cli("run", "--leg", "Pprime", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        (job,) = self.job_dirs("Pprime", "ch_intrinsics")
        netlist = self.load(job / "metrics.json")["netlist"]
        self.assertEqual((netlist["abc_depth"], netlist["abc_exchange"]), (5, "blif"))
        (self.bin / "abc_line.txt").write_text("")
        code, _, err = self.cli("run", "--leg", "Pprime", "--designs", "ch_intrinsics",
                                "--rerun")
        self.assertEqual(code, 0, err)
        netlist = self.load(self.job_dirs("Pprime", "ch_intrinsics")[0] / "metrics.json")[
            "netlist"]
        self.assertEqual((netlist["abc_depth"], netlist["abc_exchange"]), (None, None))

    def test_pprime_parmys_part(self) -> None:
        """Point (o): the Parmys stage cost comes from the latest P job's parmys.out; without a P
        job the cell is starred with the exact warning, and frontend_wall_s is frontend.time."""
        self.golden_for("ch_intrinsics")
        self.golden_for("stereovision3")
        self.assertEqual(self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")[0], 0)
        code, _, err = self.cli("run", "--leg", "Pprime", "--designs",
                                "ch_intrinsics,stereovision3")
        self.assertEqual(code, 0, err)
        self.assertIn("vtr-qor: Pprime stereovision3: no P job for the Parmys stage cost", err)
        (with_p,) = self.job_dirs("Pprime", "ch_intrinsics")
        (without,) = self.job_dirs("Pprime", "stereovision3")
        job = self.load(with_p / "job.json")
        self.assertEqual((job["parmys_stage_wall_s"], job["cost_warnings"]), (0.5, []))
        self.assertEqual(self.load(with_p / "metrics.json")["cost"]["frontend_wall_s"], 0.5)
        job = self.load(without / "job.json")
        self.assertIsNone(job["parmys_stage_wall_s"])
        self.assertEqual(job["cost_warnings"],
                         ["Pprime stereovision3: no P job for the Parmys stage cost"])


class FcheckCliTest(CliTestBase):
    """Plan Task 11: ``fcheck`` and ``run --fcheck``."""

    def fcheck_calls(self) -> list[str]:
        log = self.vtr / "fcheck-calls.log"
        return log.read_text().splitlines() if log.exists() else []

    def verdict(self, leg: str, design: str) -> str:
        (job,) = self.job_dirs(leg, design)
        return str(self.load(job / "metrics.json")["verdicts"]["fcheck"])

    def test_fcheck_existing_jobs(self) -> None:
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics,sha")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.fcheck_calls(), [])
        self.assertEqual(self.verdict("P", "sha"), "skipped")
        code, _, err = self.cli("fcheck", "--leg", "P", "--designs", "ch_intrinsics,sha")
        self.assertEqual(code, 0, err)
        calls = self.fcheck_calls()
        self.assertEqual(len(calls), 2)
        self.assertIn(f"--rtl {self.vtr}/vtr_flow/benchmarks/verilog/ch_intrinsics.v", calls[0])
        self.assertTrue(all(c.endswith("--vectors 50000 --seed 1 --verilator-j 2") for c in calls))
        self.assertEqual((self.verdict("P", "sha"), self.verdict("P", "ch_intrinsics")),
                         ("pass", "pass"))
        (job,) = self.job_dirs("P", "sha")
        self.assertFalse((job / "fcheck").exists())
        self.assertTrue((job / "fcheck.log").is_file())
        self.assertIn("vtr-qor: fcheck done: 2 pass", self.harness_lines()[-1])
        (self.vtr / "fcheck-exit").write_text("1")
        code, _, err = self.cli("fcheck", "--leg", "P", "--designs", "sha")
        self.assertEqual(code, 1, err)
        self.assertEqual(self.verdict("P", "sha"), "fail")
        self.assertTrue((job / "fcheck").is_dir())

    def test_fcheck_missing_job_and_options(self) -> None:
        code, _, err = self.cli("fcheck", "--leg", "P", "--designs", "spree")
        self.assertEqual(code, 1)
        self.assertIn("vtr-qor: fcheck: P spree: no ok job; run it first\n", err)
        code, _, err = self.cli("fcheck", "--leg", "P", "--set", "smoke", "--xl")
        self.assertEqual(code, 2)
        self.assertIn("--xl is only for run --designs", err)
        code, _, err = self.cli("fcheck", "--leg", "P")
        self.assertEqual((code, err), (2, "vtr-qor: run needs --set or --designs\n"))

    def test_run_with_fcheck(self) -> None:
        code, _, err = self.cli("run", "--leg", "P,O", "--designs", "ch_intrinsics", "--fcheck",
                                "--keep-netlists")
        self.assertEqual(code, 0, err)
        self.assertEqual(len(self.fcheck_calls()), 2)
        self.assertEqual((self.verdict("P", "ch_intrinsics"), self.verdict("O", "ch_intrinsics")),
                         ("pass", "pass"))
        # O without --keep-netlists has nothing to check: error, and the run exits 1 (review M4).
        code, _, err = self.cli("run", "--leg", "O", "--designs", "sha", "--fcheck")
        self.assertEqual(code, 1, err)
        self.assertEqual(self.verdict("O", "sha"), "error")
        self.assertIn("vtr-qor: fcheck: O sha: no kept netlist (run with --keep-netlists)", err)

    def test_run_fcheck_mismatch_is_the_exit_code(self) -> None:
        """Review M4: ``run --fcheck`` exits 1 for a mismatch (fail), 0 for a pass."""
        (self.vtr / "fcheck-exit").write_text("1")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics", "--fcheck")
        self.assertEqual(code, 1, err)
        self.assertEqual(self.verdict("P", "ch_intrinsics"), "fail")
        (self.vtr / "fcheck-exit").write_text("0")
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics", "--fcheck")
        self.assertEqual(code, 1, err)  # the cached job keeps its recorded verdict: still exit 1
        self.assertEqual(self.verdict("P", "ch_intrinsics"), "fail")

    def test_run_fcheck_on_a_cached_job_without_one(self) -> None:
        """Review M4: a job first run without ``--fcheck`` gets its fcheck on the next
        ``run --fcheck`` instead of a silent no-op."""
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.fcheck_calls(), [])
        code, _, err = self.cli("run", "--leg", "P", "--designs", "ch_intrinsics", "--fcheck")
        self.assertEqual(code, 0, err)
        self.assertIn("cached", err)
        self.assertEqual(len(self.fcheck_calls()), 1)
        self.assertEqual(self.verdict("P", "ch_intrinsics"), "pass")


class CommittedInputsTest(unittest.TestCase):
    """The committed gate inputs under tests/vtr/qor."""

    def test_equiv_exceptions_file(self) -> None:
        rows = qor_table.load_tsv(REPO_ROOT / "tests" / "vtr" / "qor" / "equiv-exceptions.tsv")
        self.assertEqual([r[0] for r in rows], ["stereovision3", "diffeq1", "diffeq2",
                                                "stereovision1", "stereovision2", "bgm"])
        self.assertEqual(rows[0][1:], ("multiclock", "two clocks (tm3_clk_v0, tm3_clk_v2): the "
                                                     "Verilator check stands in"))
        counts = {"diffeq1": 5, "diffeq2": 5, "stereovision1": 152, "stereovision2": 468,
                  "bgm": 11}
        for design, cls, text in rows[1:]:
            self.assertEqual(cls, "multiplier")
            self.assertEqual(text, f"{counts[design]} multiply, inlined by equiv-check as "
                                   "36x36 logic: the Verilator check stands in")
        for design, _, _ in rows:
            self.assertIn(design, SETS.gate)
        for absent in ("sha", "stereovision0", "blob_merge"):
            self.assertNotIn(absent, [r[0] for r in rows])

    def test_divergences_rows(self) -> None:
        rows = qor_table.load_tsv(REPO_ROOT / "tests" / "vtr" / "qor" / "divergences.tsv")
        self.assertEqual(rows[0], ("ch_intrinsics", "mem-init-dropped",
                                   "decision 6 step 5: hardmap drops RAM initial values (3C)"))
        classes = {"mem-init-dropped", "async-reset-made-sync", "baseline-fails", "dual-write-race"}
        for design, cls, text in rows:
            self.assertIn(design, SETS.designs)
            self.assertIn(cls, classes)
            if cls == "baseline-fails":
                self.assertIn("fcheck.log", text)  # each such row cites P's log


class EquivCliTest(CliTestBase):
    """Plan Task 12: ``equiv`` on O' and O jobs kept with --keep-netlists."""

    def setUp(self) -> None:
        super().setUp()
        self.tool = _script(self.repo / "tools" / "equiv-check" / "equiv-check", (
            "import sys\nfrom pathlib import Path\nhere = Path(__file__).parent\n"
            "(here / 'calls').open('a').write(' '.join(sys.argv[1:]) + '\\n')\n"
            "mode = here / 'mode'\nsys.exit(int(mode.read_text()) if mode.exists() else 0)\n"))

    def verdict(self, design: str) -> str:
        (job,) = self.job_dirs("O", design)
        return str(self.load(job / "metrics.json")["verdicts"]["equiv"])

    def test_equiv_command_line(self) -> None:
        code, _, err = self.cli("run", "--leg", "O,Oprime", "--designs", "ch_intrinsics,sha",
                                "--keep-netlists")
        self.assertEqual(code, 0, err)
        code, _, err = self.cli("equiv", "--designs", "ch_intrinsics,sha")
        self.assertEqual(code, 0, err)
        calls = (self.tool.parent / "calls").read_text().splitlines()
        self.assertEqual(len(calls), 2)
        (o_job,) = self.job_dirs("O", "sha")
        (p_job,) = self.job_dirs("Oprime", "sha")
        self.assertEqual(calls[1], f"--timeout 600 {p_job}/sha.blif {o_job}/sha.blif")
        self.assertEqual((self.verdict("sha"), self.verdict("ch_intrinsics")),
                         ("equivalent", "equivalent"))
        self.assertIn("vtr-qor: equiv done: 2 equivalent", self.harness_lines()[-1])
        (self.tool.parent / "mode").write_text("1")
        code, _, err = self.cli("equiv", "--designs", "sha")
        self.assertEqual(code, 1, err)
        self.assertEqual(self.verdict("sha"), "differs")
        (self.tool.parent / "mode").write_text("4")
        self.assertEqual(self.cli("equiv", "--designs", "sha")[0], 3)

    def test_equiv_missing_jobs(self) -> None:
        code, _, err = self.cli("run", "--leg", "O", "--designs", "sha", "--keep-netlists")
        self.assertEqual(code, 0, err)
        code, _, err = self.cli("equiv", "--designs", "sha")
        self.assertEqual(code, 1)
        self.assertIn("vtr-qor: equiv: Oprime sha: no ok job; run it first\n", err)
        self.assertIn("vtr-qor: equiv: O sha: no ok O' job to compare with\n", err)
        self.assertFalse((self.tool.parent / "calls").exists())


class StatusCleanTest(CliTestBase):
    def make(self, key12: str, design: str, status: str, when: float) -> Path:
        job = self.out / self.pin[:12] / K6 / "P" / key12 / design
        job.mkdir(parents=True)
        (job / "status").write_text(f"{status}\n")
        (job / "run").mkdir()
        if status == "ok":
            (job / "metrics.json").write_text(json.dumps({"status": "ok"}))
        (job.parent / "planned.json").write_text(json.dumps({"designs": [design]}))
        for path in (job / "status", job):
            os.utime(path, (when, when))
        return job

    def test_status_lines(self) -> None:
        now = time.time()
        self.make("aaaaaaaaaaaa", "sha", "ok", now - 3 * 86400)
        self.make("aaaaaaaaaaaa", "ch_intrinsics", "capped", now - 5 * 86400)
        self.make("bbbbbbbbbbbb", "sha", "timeout", now - 86400 / 2)
        code, out, _ = self.cli("status")
        self.assertEqual(code, 0)
        self.assertEqual(out, f"{K6} P latest - ok 1 failed 2 age 0d\n")
        (self.out / self.pin[:12] / K6 / "P" / "latest").write_text("aaaaaaaaaaaa\n")
        code, out, _ = self.cli("status")
        self.assertEqual(out, f"{K6} P latest aaaaaaaaaaaa ok 1 failed 1 age 3d\n")

    def test_clean_keep(self) -> None:
        now = time.time()
        for i, key in enumerate(("aaaaaaaaaaaa", "bbbbbbbbbbbb", "cccccccccccc")):
            self.make(key, "ch_intrinsics", "ok", now - 100 * (3 - i))
        leg = self.out / self.pin[:12] / K6 / "P"
        code, _, err = self.cli("clean", "--keep", "1")
        self.assertEqual(code, 0, err)
        self.assertEqual(sorted(p.name for p in leg.iterdir()), ["cccccccccccc"])
        self.assertIn("2 keys and 0 failed run directories removed", err)

    def test_status_while_a_run_churns(self) -> None:
        """Review M1 (the reviewer's reproduction): job directories created and deleted while
        status reads them never crash it."""
        import threading
        leg = self.out / self.pin[:12] / K6 / "P"
        self.make("aaaaaaaaaaaa", "sha", "ok", time.time())
        stop = threading.Event()

        def churn() -> None:
            job = leg / "bbbbbbbbbbbb" / "ch_intrinsics"
            while not stop.is_set():
                job.mkdir(parents=True, exist_ok=True)
                (job / "status").write_text("ok\n")
                (leg / "latest").write_text("bbbbbbbbbbbb\n")
                (job / "status").unlink()
                job.rmdir()
                job.parent.rmdir()
                (leg / "latest").unlink()

        thread = threading.Thread(target=churn)
        thread.start()
        try:
            end = time.monotonic() + 1.0
            while time.monotonic() < end:
                vtr_qor.status_line(K6, "P", leg, time.time())
        finally:
            stop.set()
            thread.join()

    def test_os_error_is_a_message(self) -> None:
        """An OSError outside the runner's handling prints vtr-qor: <message>, exit 2."""
        def broken(args: object, argv: object) -> int:
            raise PermissionError(13, "Permission denied", "/x/status")

        with mock.patch.dict(vtr_qor.COMMANDS, {"status": broken}):
            code, _, err = self.cli("status")
        self.assertEqual((code, err), (2, "vtr-qor: [Errno 13] Permission denied: '/x/status'\n"))

    def test_latest_files(self) -> None:
        leg = self.out / "leg"
        leg.mkdir(parents=True)
        (leg / "latest").write_text("not a key\n")
        self.assertIsNone(vtr_qor.read_latest(leg))
        (leg / "latest").unlink()
        (leg / "latest").symlink_to(self.tmp / "elsewhere")
        self.assertIsNone(vtr_qor.read_latest(leg))
        with self.assertRaisesRegex(qor_heavy.HeavyError, "is a symlink"):
            vtr_qor.write_latest(leg, "aaaaaaaaaaaa")
        self.assertFalse((self.tmp / "elsewhere").exists())

    def test_clean_refuses_symlink(self) -> None:
        self.make("aaaaaaaaaaaa", "sha", "ok", time.time())
        (self.out / self.pin[:12] / K6 / "P" / "latest").symlink_to(self.tmp)
        code, _, err = self.cli("clean")
        self.assertEqual(code, 2)
        self.assertIn("is a symlink", err)


class DoctorTest(CliTestBase):
    def setUp(self) -> None:
        super().setUp()
        # doctor run by hand: not inside a heavy run (no descriptor 9 on the lock).
        self.stack.enter_context(mock.patch.object(qor_heavy, "FD9", self.tmp / "no-fd9"))

    def doctor(self) -> tuple[int, list[str]]:
        code, out, err = self.cli("doctor")
        self.assertEqual(err, "")
        return code, out.splitlines()

    def test_doctor_checklist(self) -> None:
        code, lines = self.doctor()
        self.assertEqual(code, 0, "\n".join(lines))
        items = [line.split(":")[0] for line in lines]
        self.assertEqual(items, [f"ok  {item}" for item in (
            "vtr", "vpr", "abc", "yosys", "verilator", "venv", "time", "heavy-run",
            "systemd-run", "disk", "memory", "lock", "legs")])
        self.assertIn(f"ok  vtr: {self.vtr} at {self.pin[:12]} (the pin)", lines[0])
        self.assertIn("Yosys 0.55", lines[3])
        self.assertEqual(lines[-1], "ok  legs: P Pprime Oprime O R Oprov")

        self.heavy_run.unlink()
        code, lines = self.doctor()
        self.assertEqual(code, 1)
        self.assertIn("FAIL  heavy-run: tools/heavy-run missing (2E-1 not merged)", lines)
        self.assertEqual(sum(line.startswith("FAIL") for line in lines), 1)

    def test_doctor_failures(self) -> None:
        self.heavy_run.write_text("#!/bin/sh\n# OOMPolicy=continue only in a comment\n")
        self.df_file.write_text("15\n")
        self.meminfo.write_text(f"MemAvailable: {int(6.99 * KIB_PER_GIB)} kB\n")
        (self.vtr / "README").write_text("edited\n")
        self.set_help(("read_techlib", "read_blif", "write_blif"))
        (self.vtr / "build" / "bin" / "yosys").write_text('#!/bin/sh\necho "Yosys 0.56"\n')
        no_time = _shell(self.tmp / "no-time", "exec \"$@\"\n")  # runs, reports nothing
        with open(self.lock, "a") as holder, \
                mock.patch.object(qor_legs, "TIME_BIN", str(no_time)):
            fcntl.flock(holder, fcntl.LOCK_EX | fcntl.LOCK_NB)
            code, lines = self.doctor()
        self.assertEqual(code, 1)
        failed = {line.split(":")[0]: line for line in lines if line.startswith("FAIL")}
        self.assertEqual(set(failed), {f"FAIL  {item}" for item in (
            "vtr", "yosys", "time", "heavy-run", "disk", "memory", "lock")})
        self.assertIn("has 2 modified tracked files", failed["FAIL  vtr"])  # README, yosys
        self.assertIn("does not pass -p OOMPolicy=continue (PR #58 not merged)",
                      failed["FAIL  heavy-run"])
        self.assertEqual(failed["FAIL  disk"], "FAIL  disk: C: has 15 GiB free (< 20 GiB)")
        self.assertEqual(failed["FAIL  memory"], "FAIL  memory: MemAvailable 6.9 GiB (< 7)")
        self.assertEqual(failed["FAIL  lock"], f"FAIL  lock: {self.lock} is busy (WRITE lock held "
                                               f"by pid {os.getpid()}: another heavy run)")
        self.assertIn("is Yosys 0.56, ORACLES.md pins Yosys 0.55", failed["FAIL  yosys"])
        self.assertIn("ok  legs: P R; Pprime missing 'abc'; Oprime missing 'read_verilog'; "
                      "O missing 'read_verilog'; Oprov missing 'read_verilog'", lines)

    def test_doctor_more_failures(self) -> None:
        """I2: every other item's FAIL: the systemd probe, verilator, an unexecutable vpr and
        heavy-run."""
        (self.vtr / "vpr" / "vpr").chmod(0o644)
        self.heavy_run.chmod(0o644)
        (self.bin / "verilator").unlink()
        empty = self.tmp / "empty-bin"  # PATH without verilator (true for the fake time)
        empty.mkdir()
        (empty / "true").symlink_to(shutil.which("true") or "/bin/true")
        false = shutil.which("false") or "/bin/false"
        with mock.patch.object(vtr_qor, "SYSTEMD_PROBE", (false,)), \
                mock.patch.dict(os.environ, {"PATH": str(empty)}):
            code, lines = self.doctor()
        self.assertEqual(code, 1)
        failed = {line.split(":")[0]: line for line in lines if line.startswith("FAIL")}
        self.assertEqual(set(failed), {f"FAIL  {item}" for item in (
            "vtr", "vpr", "verilator", "heavy-run", "systemd-run")})  # vtr: vpr's mode bit
        self.assertEqual(failed["FAIL  vpr"], f"FAIL  vpr: {self.vtr / 'vpr' / 'vpr'} missing")
        self.assertEqual(failed["FAIL  verilator"], "FAIL  verilator: verilator is not on PATH")
        self.assertEqual(failed["FAIL  heavy-run"],
                         f"FAIL  heavy-run: {self.heavy_run} is not executable")
        self.assertEqual(failed["FAIL  systemd-run"], f"FAIL  systemd-run: {false} failed (exit 1)")

    def test_doctor_lock_holders(self) -> None:
        """Review M3: inside a heavy run, the run's own lock is ok (decided from the cgroup and
        descriptor 9); a build's shared lock is ok; the variable alone proves nothing."""
        with open(self.lock, "a") as holder:
            fcntl.flock(holder, fcntl.LOCK_EX)
            self.assertEqual(vtr_qor._lock_item()[0], False)  # env says slice; no fd 9
            elsewhere = self.tmp / "fd9-elsewhere"
            elsewhere.symlink_to(self.tmp / "self-cgroup")  # descriptor 9 on another file
            with mock.patch.object(qor_heavy, "FD9", elsewhere):
                self.assertEqual(vtr_qor._lock_item()[0], False)
            fd9 = self.tmp / "fd9"
            with mock.patch.object(qor_heavy, "FD9", fd9):
                self.assertEqual(vtr_qor._lock_item(),
                                 (True, f"{self.lock} held by this heavy run "
                                        "(odin3-heavy.slice)"))
                outside = self.tmp / "outside-cgroup"
                outside.write_text("0::/user.slice/app.slice/term.scope\n")
                with mock.patch.object(qor_heavy, "SELF_CGROUP", outside):
                    self.assertEqual(vtr_qor._lock_item()[0], False)  # fd 9 alone neither
            fcntl.flock(holder, fcntl.LOCK_UN)
            fcntl.flock(holder, fcntl.LOCK_SH)
            ok, detail = vtr_qor._lock_item()
        self.assertTrue(ok)
        self.assertIn("shared by a build (READ lock", detail)

    def test_doctor_without_vtr(self) -> None:
        code, out, _ = run_main(vtr_qor.main, ["doctor", "--vtr-root", str(self.tmp / "none"),
                                               "--odin3", str(self.odin3)])
        self.assertEqual(code, 1)
        failed = [line.split(":")[0] for line in out.splitlines() if line.startswith("FAIL")]
        self.assertEqual(failed, [f"FAIL  {item}" for item in ("vtr", "vpr", "abc", "yosys",
                                                               "venv")])

    def test_lock_probe_never_takes_the_lock(self) -> None:
        """The /proc/locks probe sees a holder, and leaves the lock free when nobody holds it
        (no flock of its own: another process can take it right after)."""
        self.assertEqual(vtr_qor.lock_holders(self.lock), [])
        self.assertEqual(vtr_qor.lock_holders(self.tmp / "absent.lock"), [])
        with open(self.lock, "a") as holder:
            fcntl.flock(holder, fcntl.LOCK_SH)
            self.assertEqual(vtr_qor.lock_holders(self.lock), [("READ", str(os.getpid()))])



class PinsTest(unittest.TestCase):
    def test_oracles_pins(self) -> None:
        """The pins doctor checks are ORACLES.md's."""
        text = (REPO_ROOT / "docs" / "ORACLES.md").read_text()
        self.assertIn(f"`{qor_legs.VTR_PIN}`", text)
        self.assertIn(f"{vtr_qor.YOSYS_PIN},", text)


# -- T8: the first real run (manual) -----------------------------------------------------------

T8_DESIGNS = ("ch_intrinsics", "stereovision3", "diffeq2")


@unittest.skipUnless(os.environ.get("ODIN3_QOR_T8") == "1",
                     "manual: T8 checks a finished real run (set ODIN3_QOR_T8=1)")
class T8Check(unittest.TestCase):
    """T8 (plan Task 6), after the controller's real run of
    ``vtr-qor run --leg P --designs ch_intrinsics,stereovision3,diffeq2`` through heavy-run:
    3 jobs ok with vpr_status=success; each metrics.json's packed and routed values equal its
    run/parse_results.txt; run/ holds only the keep list plus the gzipped pre-VPR netlist.
    Read-only. The work tree is ``$ODIN3_QOR_DIR``, else ``~/odin3-ws/work/vtr-qor``."""

    def test_t8(self) -> None:
        home = Path(pwd.getpwuid(os.getuid()).pw_dir)
        out = Path(os.environ.get("ODIN3_QOR_DIR") or home / "odin3-ws/work/vtr-qor")
        leg_dir = out / qor_legs.VTR_PIN[:12] / K6 / "P"
        for design in T8_DESIGNS:
            with self.subTest(design=design):
                jobs = sorted(leg_dir.glob(f"*/{design}/status"), key=lambda p: p.stat().st_mtime)
                self.assertTrue(jobs, f"no P job for {design} under {leg_dir}")
                self.check_job(jobs[-1].parent, design)
        done = [line for line in (out / "harness.log").read_text().splitlines()
                if line.startswith("vtr-qor: done:")]
        self.assertTrue(done)
        print(f"\nT8 {done[-1]}", file=sys.stderr)

    def check_job(self, job: Path, design: str) -> None:
        self.assertEqual((job / "status").read_text().strip(), "ok")
        metrics = json.loads((job / "metrics.json").read_text())
        self.assertEqual((metrics["status"], metrics["routed"]["vpr_status"]), ("ok", "success"))
        run = job / "run"
        parse = qor_parse.read_parse_results(run / "parse_results.txt", not_before=0, label="t8")
        qor = qor_parse.read_parse_results(run / "qor_results.txt", not_before=0, label="t8")
        expected = qor_parse.metrics_from_parse(parse, qor)
        self.assertEqual(metrics["routed"], expected["routed"])
        self.assertEqual(metrics["packed"], expected["packed"])
        self.assertIsNotNone(metrics["routed"]["min_chan_width"])
        names = {p.name for p in run.iterdir()}
        gz = f"{design}.pre-vpr.blif.gz"
        self.assertIn(gz, names)
        stray = [n for n in names - {gz}
                 if not any(fnmatch.fnmatchcase(n, k) for k in qor_sched.KEEP_RUN_FILES)]
        self.assertEqual(stray, [])


if __name__ == "__main__":
    unittest.main()

"""Tests for tools/proc-check/proc-diff (2E spec §18.3, §18.4, §18.7; plan points (l), (o), (q),
(t)) over fake odin3, yosys and memcap scripts that log their argv."""

from __future__ import annotations

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from unittest import mock

import proc_diff

from . import helpers

ORACLE = "Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0 -fPIC -O3)"

# The fake odin3 writes every `write_rtlil P` of its -p script: in.il a fixed module, any other
# file $FAKE_AIL (default: no process). $FAKE_ODIN3_RC and $FAKE_ODIN3_MSG make it fail.
FAKE_ODIN3 = """#!{python}
import json, os, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["odin3", *argv]) + "\\n")
rc = int(os.environ.get("FAKE_ODIN3_RC", "0"))
if rc:
    print(os.environ.get("FAKE_ODIN3_MSG", "odin3: error: no"), file=sys.stderr)
    sys.exit(rc)
script = argv[argv.index("-p") + 1]
for cmd in (c.strip() for c in script.split(";")):
    if cmd.startswith("write_rtlil "):
        path = cmd.split()[1]
        text = "module \\\\top\\nend\\n"
        if not path.endswith("/in.il"):
            text = os.environ.get("FAKE_AIL", text)
        with open(path, "w") as out:
            out.write(text)
"""

# The fake Yosys copies $FAKE_BLIF_DIR/<stem of the read_rtlil file>.blif (else default.blif) to
# the write_blif target. $FAKE_YOSYS_FAIL=<stem> makes that run print $FAKE_YOSYS_MSG and exit 1.
FAKE_YOSYS = """#!{python}
import json, os, shutil, sys
argv = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write(json.dumps(["yosys", *argv]) + "\\n")
if argv == ["-V"]:
    print(os.environ.get("FAKE_YOSYS_VERSION", "{oracle}"))
    sys.exit(0)
cmds = [c.strip() for c in argv[argv.index("-p") + 1].split(";")]
src = next(c.split()[1] for c in cmds if c.startswith("read_rtlil "))
out = next(c.split()[-1] for c in cmds if c.startswith("write_blif "))
stem = os.path.splitext(os.path.basename(src))[0]
if os.environ.get("FAKE_YOSYS_FAIL") == stem:
    print(os.environ.get("FAKE_YOSYS_MSG", "ERROR: no"))
    sys.exit(1)
blifs = os.environ["FAKE_BLIF_DIR"]
pick = os.path.join(blifs, stem + ".blif")
shutil.copyfile(pick if os.path.exists(pick) else os.path.join(blifs, "default.blif"), out)
"""

FAKE_MEMCAP = """import json, os, subprocess

def run(argv, cap, capture_output=False):
    with open(os.environ["FAKE_MEMCAP_LOG"], "a") as log:
        log.write(json.dumps({"argv": list(argv), "cap": cap}) + "\\n")
    return subprocess.run(argv, capture_output=capture_output, text=True, check=False)
"""

FAKE_ELAB_SWEEP = """from pathlib import Path

def collect(paths):
    files = [f for p in paths for f in sorted(Path(p).rglob("*.v"))]
    return sorted(files, key=lambda f: -f.stat().st_size)
"""

DFF_BLIF = """.model top
.inputs clk d
.outputs q
.subckt $dff CLK=clk D=d Q=q
.param CLK_POLARITY {pol}
.param WIDTH 00000000000000000000000000000001
.end
"""

GIB = 1 << 30


def _exe(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


class ProcDiffTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name).resolve()
        self.odin3 = _exe(self.dir / "odin3", FAKE_ODIN3.replace("{python}", sys.executable))
        self.yosys = _exe(self.dir / "yosys", FAKE_YOSYS.replace("{python}", sys.executable)
                          .replace("{oracle}", ORACLE))
        self.memcap = self.dir / "memcap.py"
        self.memcap.write_text(FAKE_MEMCAP, encoding="utf-8")
        self.blifs = self.dir / "blifs"
        self.blifs.mkdir()
        self.set_blif("default", DFF_BLIF.format(pol="1"))
        self.rows = self.dir / "rows"
        self.rows.mkdir()
        self.src = self.rows / "t.v"
        self.src.write_text("module top; endmodule\n", encoding="utf-8")
        self.keep = self.dir / "keep"
        self.log = self.dir / "argv.log"
        self.memlog = self.dir / "memcap.log"
        env = {"FAKE_LOG": str(self.log), "FAKE_MEMCAP_LOG": str(self.memlog),
               "FAKE_BLIF_DIR": str(self.blifs)}
        env_patch = mock.patch.dict(os.environ, env)
        env_patch.start()
        self.addCleanup(env_patch.stop)
        for name, value in (("MEMCAP_PY", self.memcap),
                            ("ELAB_SWEEP_PY", self.dir / "no_elab_sweep.py")):
            patch = mock.patch.object(proc_diff, name, value)
            patch.start()
            self.addCleanup(patch.stop)

    # -- helpers ---------------------------------------------------------------------------

    def set_blif(self, stem: str, text: str) -> None:
        (self.blifs / f"{stem}.blif").write_text(text, encoding="utf-8")

    def run_tool(self, *args: str, inputs: tuple[str, ...] | None = None,
                 env: dict[str, str] | None = None) -> tuple[int, str, str]:
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), "--keep",
                str(self.keep), *args, *(inputs if inputs is not None else (str(self.rows),))]
        with mock.patch.dict(os.environ, env or {}):
            return helpers.run_main(proc_diff.main, argv)

    def children(self) -> list[dict[str, object]]:
        if not self.memlog.exists():
            return []
        return [json.loads(line) for line in self.memlog.read_text().splitlines()]

    def argvs(self) -> list[list[str]]:
        return [list(map(str, c["argv"])) for c in self.children()]  # type: ignore[call-overload]

    def scripts(self) -> list[str]:
        """The -p script of every odin3 and yosys child (not -V, not netlist-compare)."""
        return [argv[argv.index("-p") + 1] for argv in self.argvs() if "-p" in argv]

    def sub(self, text: str, work: str = "t") -> str:
        return text.replace("{F}", str(self.src)).replace("{W}", str(self.keep / work))

    # -- the command strings ---------------------------------------------------------------

    def test_default_commands_exactly(self) -> None:
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        argvs = self.argvs()
        self.assertEqual(argvs[0], [str(self.yosys), "-V"])
        self.assertEqual(argvs[1], [str(self.odin3), "-p", self.sub(
            "read_verilog {F}; opt_expr; opt_clean; opt -nodffe -nosdff; "
            "write_rtlil {W}/in.il; proc --noopt; write_rtlil {W}/a.il")])
        self.assertEqual(argvs[2], [str(self.yosys), "-p", self.sub(
            "read_rtlil {W}/in.il; hierarchy -auto-top; proc -norom -noopt; opt_clean -purge; "
            "write_blif -blackbox -icells -param {W}/b.blif")])
        self.assertEqual(argvs[3], [str(self.yosys), "-p", self.sub(
            "read_rtlil {W}/a.il; hierarchy -auto-top; opt_clean -purge; "
            "write_blif -blackbox -icells -param {W}/a.blif")])
        self.assertEqual(argvs[4], [sys.executable, str(proc_diff.NETLIST_COMPARE_PY),
                                    "--implicit-models", self.sub("{W}/a.norm.blif"),
                                    self.sub("{W}/b.norm.blif")])
        self.assertEqual(len(argvs), 5)
        self.assertIn("t.v: identical", out)

    def test_top_no_trio_with_opt_nolatches(self) -> None:
        code, _, _ = self.run_tool("--top", "top", "--no-trio", "--with-opt", "--nolatches")
        self.assertEqual(code, 0)
        argvs = self.argvs()
        self.assertEqual(argvs[1], [str(self.odin3), "--top", "top", "-p", self.sub(
            "read_verilog --nolatches {F}; write_rtlil {W}/in.il; proc; write_rtlil {W}/a.il")])
        self.assertEqual(self.scripts()[1:], [
            self.sub("read_rtlil {W}/in.il; hierarchy -top top; proc -norom; opt_clean -purge; "
                     "write_blif -blackbox -icells -param {W}/b.blif"),
            self.sub("read_rtlil {W}/a.il; hierarchy -top top; opt_clean -purge; "
                     "write_blif -blackbox -icells -param {W}/a.blif"),
        ])

    def test_each_option_alone(self) -> None:
        cases = {
            "--top": ("read_verilog {F}; opt_expr; opt_clean; opt -nodffe -nosdff; write_rtlil "
                      "{W}/in.il; proc --noopt; write_rtlil {W}/a.il"),
            "--no-trio": "read_verilog {F}; write_rtlil {W}/in.il; proc --noopt; "
                         "write_rtlil {W}/a.il",
            "--with-opt": ("read_verilog {F}; opt_expr; opt_clean; opt -nodffe -nosdff; "
                           "write_rtlil {W}/in.il; proc; write_rtlil {W}/a.il"),
            "--nolatches": ("read_verilog --nolatches {F}; opt_expr; opt_clean; "
                            "opt -nodffe -nosdff; write_rtlil {W}/in.il; proc --noopt; "
                            "write_rtlil {W}/a.il"),
        }
        for option, script in cases.items():
            with self.subTest(option):
                self.memlog.unlink(missing_ok=True)
                args = ("--top", "m") if option == "--top" else (option,)
                self.assertEqual(self.run_tool(*args)[0], 0)
                self.assertEqual(self.scripts()[0], self.sub(script))
                b_proc = "proc -norom" if option == "--with-opt" else "proc -norom -noopt"
                hier = "hierarchy -top m" if option == "--top" else "hierarchy -auto-top"
                self.assertEqual(self.scripts()[1], self.sub(
                    f"read_rtlil {{W}}/in.il; {hier}; {b_proc}; opt_clean -purge; "
                    "write_blif -blackbox -icells -param {W}/b.blif"))

    def test_mem_cap_reaches_every_child(self) -> None:
        self.assertEqual(self.run_tool("--mem-cap", "2G")[0], 0)
        self.assertEqual({c["cap"] for c in self.children()}, {2 * GIB})
        self.memlog.unlink()
        self.assertEqual(self.run_tool()[0], 0)
        self.assertEqual({c["cap"] for c in self.children()}, {4 * GIB})
        self.assertEqual(self.run_tool("--mem-cap", "3X")[0], 2)

    # -- normalisation ---------------------------------------------------------------------

    def test_int_params_become_decimal(self) -> None:
        ints = proc_diff.load_int_params()
        text = (".subckt $adff ARST=r CLK=c D=d Q=q\n"
                ".param CLK_POLARITY 00000000000000000000000000000001\n"
                ".param ARST_POLARITY 1\n.param ARST_VALUE 0101\n.param WIDTH 100\n"
                ".param A_SIGNED 0\n.param WIDTH 1x\n.param WIDTH " + "0" * 32 + "1\n")
        self.assertEqual(proc_diff.normalise_text(text, ints), (
            ".subckt $adff ARST=r CLK=c D=d Q=q\n.param CLK_POLARITY 1\n"
            ".param ARST_POLARITY 1\n.param ARST_VALUE 0101\n.param WIDTH 4\n"
            ".param A_SIGNED 0\n.param WIDTH 1x\n.param WIDTH " + "0" * 32 + "1\n"))
        self.assertIn("CLK_POLARITY", ints)
        self.assertNotIn("ARST_VALUE", ints)

    def test_32_bit_and_1_bit_polarities_compare_equal(self) -> None:
        self.set_blif("in", DFF_BLIF.format(pol="1"))
        self.set_blif("a", DFF_BLIF.format(pol="00000000000000000000000000000001"))
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        a_blif = (self.keep / "t" / "a.norm.blif").read_text()
        self.assertIn(".param CLK_POLARITY 1\n", a_blif)
        self.assertIn(".param WIDTH 1\n", a_blif)

    def test_connect_order_is_a_set(self) -> None:
        """§20 #8: connect lines across processes in either order give the same netlist."""
        head = ".model top\n.inputs a\n.outputs y1 y2\n"
        self.set_blif("in", head + ".names a y1\n1 1\n.names y1 y2\n1 1\n.end\n")
        self.set_blif("a", head + ".names a y2\n1 1\n.names y2 y1\n1 1\n.end\n")
        code, out, err = self.run_tool()
        self.assertEqual(code, 0, out + err)
        text = ".model top\n.names a b\n1 1\n.names b c\n1 1\n.names c d\n1 1\n.names x y\n" \
               "10 1\n.end\n"
        self.assertEqual(proc_diff.normalise_text(text, frozenset()),
                         ".model top\n.names a b\n1 1\n.names a c\n1 1\n.names a d\n1 1\n"
                         ".names x y\n10 1\n.end\n")

    def test_reduce_or_input_order(self) -> None:
        """§20 #8: a hold's $reduce_or inputs in another order are not a difference."""
        head = ".model top\n.inputs a b\n.outputs y\n"
        self.set_blif("in", head + ".subckt $reduce_or A[0]=a A[1]=b Y=y\n.end\n")
        self.set_blif("a", head + ".subckt $reduce_or A[0]=b A[1]=a Y=y\n.end\n")
        self.assertEqual(self.run_tool()[0], 0)

    # -- exit mapping ----------------------------------------------------------------------

    def test_exit_mapping(self) -> None:
        self.assertEqual(self.run_tool()[0], 0)
        self.assertFalse((self.keep / "t" / "diff.txt").exists())
        self.set_blif("a", DFF_BLIF.format(pol="0"))
        code, out, _ = self.run_tool()
        self.assertEqual(code, 1)
        self.assertIn("t.v: differs", out)
        self.assertIn(f"files kept in {self.keep / 't'}", out)
        self.assertIn("CLK_POLARITY", (self.keep / "t" / "diff.txt").read_text())
        code, out, err = self.run_tool(env={"FAKE_ODIN3_RC": "1"})
        self.assertEqual(code, 2)
        self.assertIn("t.v: error", out)
        self.assertIn("proc-diff: odin3 exited 1 (step A)", err)
        code, _, err = self.run_tool(env={"FAKE_YOSYS_FAIL": "a"})
        self.assertEqual(code, 2)
        self.assertIn("yosys exited 1 (step A-BLIF)", err)
        self.set_blif("a", "not blif\n")
        code, _, err = self.run_tool()
        self.assertEqual(code, 2)
        self.assertIn("netlist-compare exited 2 (step compare)", err)

    def test_difference_without_keep_keeps_a_temp_dir(self) -> None:
        self.set_blif("a", DFF_BLIF.format(pol="0"))
        argv = ["--odin3", str(self.odin3), "--yosys", str(self.yosys), str(self.rows)]
        code, out, _ = helpers.run_main(proc_diff.main, argv)
        self.assertEqual(code, 1)
        kept = Path(out.split("files kept in ")[1].split()[0])
        self.addCleanup(shutil.rmtree, kept, True)
        self.assertTrue((kept / "diff.txt").is_file())

    def test_missing_blif_is_an_error(self) -> None:
        with mock.patch.object(proc_diff, "_yosys_argv", self.no_a_blif(proc_diff._yosys_argv)):
            code, out, err = self.run_tool()
        self.assertEqual(code, 2, out + err)
        self.assertIn("netlist-compare exited 2 (step compare)", err)
        self.assertIn("cannot read", out)

    def no_a_blif(self, real: Callable[..., list[str]]) -> Callable[..., list[str]]:
        """Side A's Yosys run writes its BLIF elsewhere, so a.blif is never made."""
        def build(opts: proc_diff.Options, rtlil: Path, passes: list[str],
                  blif_out: Path) -> list[str]:
            out = blif_out.with_name("other.blif") if blif_out.name == "a.blif" else blif_out
            return real(opts, rtlil, passes, out)
        return build

    def test_memcap_refusal_is_a_run_error(self) -> None:
        self.memcap.write_text("def run(argv, cap):\n    raise ValueError('cap too high')\n",
                               encoding="utf-8")
        code, _, err = self.run_tool("--mem-cap", "999G")
        self.assertEqual(code, 2)
        self.assertIn("memcap refused", err)
        self.assertIn("cap too high", err)

    def test_a_il_holding_process_is_an_error(self) -> None:
        code, out, err = self.run_tool(env={"FAKE_AIL": "module \\top\n  process $p\n  end\nend\n"})
        self.assertEqual(code, 2)
        self.assertIn("proc-diff: a.il still holds processes", err)
        self.assertEqual(len(self.scripts()), 1)  # no Yosys run after A

    def test_memcap_missing(self) -> None:
        with mock.patch.object(proc_diff, "MEMCAP_PY", self.dir / "absent" / "memcap.py"):
            code, out, err = self.run_tool()
        self.assertEqual(code, 2)
        self.assertEqual(err, "proc-diff: tools/elab-check/memcap.py is missing (2D-6)\n")
        self.assertFalse(self.log.exists())  # no child started

    def test_version_refused_unless_allowed(self) -> None:
        env = {"FAKE_YOSYS_VERSION": "Yosys 0.69+270 (git sha1 x)"}
        code, _, err = self.run_tool(env=env)
        self.assertEqual(code, 2)
        self.assertIn("not Yosys 0.55", err)
        self.assertEqual(self.argvs(), [[str(self.yosys), "-V"]])
        self.assertEqual(self.run_tool("--allow-version", "0.69", env=env)[0], 0)

    # -- --write ---------------------------------------------------------------------------

    def test_write_refused_on_a_difference(self) -> None:
        self.set_blif("a", DFF_BLIF.format(pol="0"))
        code, _, err = self.run_tool("--write")
        self.assertEqual(code, 2)
        self.assertIn("--write refused: the run is not identical", err)
        self.assertFalse((self.rows / "t.il").exists())

    def test_write_goldens_per_point_l(self) -> None:
        code, out, _ = self.run_tool("--write", "--top", "top")
        self.assertEqual(code, 0)
        golden = self.argvs()[-1]
        self.assertEqual(golden[:4], [str(self.odin3), "--top", "top", "-p"])
        out_il = golden[4].rsplit(" ", 1)[1]
        self.assertEqual(golden[4], f"read_verilog {self.src}; proc --noopt; write_rtlil {out_il}")
        self.assertEqual((self.rows / "t.il").read_text(), "module \\top\nend\n")
        self.assertIn(f"wrote {self.rows / 't.il'}", out)
        self.assertEqual(self.run_tool("--write", "--nolatches")[0], 0)
        self.assertTrue(self.scripts()[-1].startswith(f"read_verilog --nolatches {self.src}; "
                                                      "proc --noopt; write_rtlil "))
        self.assertTrue((self.rows / "t.nolatches.il").is_file())

    # -- --bisect --------------------------------------------------------------------------

    def test_bisect_runs_four_sides_with_point_q_lists(self) -> None:
        self.set_blif("a", DFF_BLIF.format(pol="0"))
        code, out, _ = self.run_tool("--bisect", "--top", "top")
        self.assertEqual(code, 1)
        self.assertIn("t.v: differs: no phase differs", out)
        tails = {
            "clean": "proc_rmdead; proc_prune; proc_init; proc_arst; proc_mux; proc_dlatch; "
                     "proc_dff; proc_memwr; proc_clean",
            "rmdead": "proc_prune; proc_init; proc_arst; proc_mux; proc_dlatch; proc_dff; "
                      "proc_memwr; proc_clean",
            "prune": "proc_init; proc_arst; proc_mux; proc_dlatch; proc_dff; proc_memwr; "
                     "proc_clean",
            "arst": "proc_mux; proc_dlatch; proc_dff; proc_memwr; proc_clean",
        }
        want: list[str] = []
        for phase, tail in tails.items():
            want += [
                self.sub("read_verilog {F}; opt_expr; opt_clean; opt -nodffe -nosdff; "
                         f"proc --stop-after {phase}; write_rtlil {{W}}/a_{phase}.il"),
                self.sub(f"read_rtlil {{W}}/a_{phase}.il; hierarchy -top top; {tail}; "
                         "opt_clean -purge; write_blif -blackbox -icells -param "
                         f"{{W}}/a_{phase}.blif"),
            ]
        self.assertEqual(self.scripts()[3:], want)

    def test_bisect_names_the_first_phase(self) -> None:
        self.set_blif("a", DFF_BLIF.format(pol="0"))
        self.set_blif("a_rmdead", DFF_BLIF.format(pol="0"))
        code, out, _ = self.run_tool("--bisect")
        self.assertEqual(code, 1)
        self.assertIn("t.v: differs: first difference after rmdead", out)
        self.assertEqual(len(self.scripts()), 3 + 4)  # stops after rmdead's pair

    def test_bisect_with_opt_ends_with_opt_expr(self) -> None:
        self.assertEqual(proc_diff.bisect_passes("arst", True),
                         ["proc_mux", "proc_dlatch", "proc_dff", "proc_memwr", "proc_clean",
                          "opt_expr -keepdc"])

    def test_identical_run_never_bisects(self) -> None:
        self.assertEqual(self.run_tool("--bisect")[0], 0)
        self.assertEqual(len(self.scripts()), 3)

    # -- several files ---------------------------------------------------------------------

    def test_two_directories_one_ascending_order(self) -> None:
        other = self.dir / "other"
        other.mkdir()
        sizes = {self.rows / "big.v": 900, other / "small.v": 10, self.rows / "mid.v": 400,
                 other / "sub" / "tiny.v": 1}
        (other / "sub").mkdir()
        for path, size in sizes.items():
            path.write_text("x" * size, encoding="utf-8")
        (other / "notes.txt").write_text("not verilog", encoding="utf-8")
        code, out, _ = self.run_tool(inputs=(str(self.rows), str(other)))
        self.assertEqual(code, 0)
        order = [s.split("read_verilog ")[1].split(";")[0] for s in self.scripts()
                 if s.startswith("read_verilog")]
        self.assertEqual([Path(p).name for p in order], ["tiny.v", "small.v", "t.v", "mid.v",
                                                         "big.v"])
        self.assertIn("sub/tiny.v: identical", out)
        self.assertIn("proc-diff: 5 files, exit 0", out)

    def test_elab_sweep_collect_is_used_when_importable(self) -> None:
        sweep = self.dir / "elab_sweep.py"
        sweep.write_text(FAKE_ELAB_SWEEP, encoding="utf-8")
        (self.rows / "big.v").write_text("x" * 100, encoding="utf-8")
        with mock.patch.object(proc_diff, "ELAB_SWEEP_PY", sweep):
            code, out, _ = self.run_tool()
        self.assertEqual(code, 0)
        self.assertLess(out.index("big.v:"), out.index("t.v:"))  # the fake's order, largest first

    def test_missing_input(self) -> None:
        code, _, err = self.run_tool(inputs=(str(self.dir / "nope.v"),))
        self.assertEqual(code, 2)
        self.assertIn("nope.v: no such file or directory", err)

    # -- expect-fail rules -----------------------------------------------------------------

    def expect(self, text: str) -> str:
        path = self.dir / "expect.txt"
        path.write_text(text, encoding="utf-8")
        return str(path)

    def two_files(self) -> None:
        (self.rows / "u.v").write_text("module top; endmodule // u\n", encoding="utf-8")

    def test_expect_fail_rules(self) -> None:
        self.two_files()
        fail_u = {"FAKE_YOSYS_FAIL": "in", "FAKE_YOSYS_MSG": "ERROR: boom"}
        # u.v fails (both files fail B here); unlisted failures fail the run.
        self.assertEqual(self.run_tool(env=fail_u)[0], 2)
        listed = self.expect("# comment\n\nt.v Parmys rejects it\nu.v a reason\n")
        code, out, _ = self.run_tool("--expect-fail", listed, env=fail_u)
        self.assertEqual(code, 0, out)
        self.assertIn("t.v: expected failure (Parmys rejects it): error", out)
        # A listed pass fails the run.
        code, out, _ = self.run_tool("--expect-fail", listed)
        self.assertEqual(code, 1)
        self.assertIn("u.v: identical, but listed as expected to fail (a reason)", out)
        # One listed, one unlisted failure.
        code, _, _ = self.run_tool("--expect-fail", self.expect("t.v reason\n"), env=fail_u)
        self.assertEqual(code, 2)

    def test_expect_fail_malformed(self) -> None:
        code, _, err = self.run_tool("--expect-fail", self.expect("t.v\n"))
        self.assertEqual(code, 2)
        self.assertIn("expect.txt:1: 't.v' has no reason", err)
        self.assertEqual(self.run_tool("--expect-fail", str(self.dir / "none.txt"))[0], 2)

    def test_divergence_17_is_recognised(self) -> None:
        env = {"FAKE_YOSYS_FAIL": "in",
               "FAKE_YOSYS_MSG": "ERROR: Assert `ctrl_sig.size() == 1' failed in "
                                 "passes/proc/proc_mux.cc:264."}
        code, out, _ = self.run_tool(env=env)
        self.assertEqual(code, 0, out)
        self.assertIn("t.v: expected difference (§20 #17: Yosys 0.55 asserts", out)
        self.assertEqual(self.run_tool("--write", env=env)[0], 2)  # not identical
        code, out, _ = self.run_tool("--expect-fail", self.expect("t.v §20 #17 shape\n"), env=env)
        self.assertEqual(code, 0, out)

    def test_cited_divergence_must_match(self) -> None:
        self.set_blif("a", DFF_BLIF.format(pol="0"))
        cite18 = self.expect("t.v §20 #18: Yosys wrong logic\n")
        code, out, _ = self.run_tool("--expect-fail", cite18)
        self.assertEqual(code, 0, out)
        self.assertIn("t.v: expected failure (§20 #18: Yosys wrong logic): differs", out)
        # #18 is a difference: an odin3 error is not it.
        code, out, _ = self.run_tool("--expect-fail", cite18, env={"FAKE_ODIN3_RC": "1"})
        self.assertEqual(code, 1)
        self.assertIn("listed as §20 #18 (differ), but error", out)
        # #16 is Odin III's E3.
        cite16 = self.expect("t.v §20 #16 a 100-round chain\n")
        e3 = {"FAKE_ODIN3_RC": "1", "FAKE_ODIN3_MSG": "t.v:3:5: error: asynchronous reset 'r' "
              "does not settle the value of 'q' after 100 substitutions"}
        self.assertEqual(self.run_tool("--expect-fail", cite16, env=e3)[0], 0)
        self.assertEqual(self.run_tool("--expect-fail", cite16)[0], 1)  # a difference instead
        # An uncited reason accepts any failure.
        self.assertEqual(self.run_tool("--expect-fail", self.expect("t.v §20 #1 merged\n"))[0],
                         0)

    def test_cited(self) -> None:
        self.assertEqual(proc_diff.cited("a §20 #18 shape"), 18)
        self.assertIsNone(proc_diff.cited("§20 #7 cap"))
        self.assertIsNone(proc_diff.cited("#18 without the section"))
        self.assertEqual(set(proc_diff.DIVERGENCES), {8, 9, 16, 17, 18})

    # -- fix round 1 -------------------------------------------------------------------------

    def test_bisect_normalises_from_the_originals(self) -> None:
        """I1: a decimal 10 in a normalised file must not be re-read as binary (10 -> 2)."""
        ten = "0" * 28 + "1010"
        body = ".model top\n.inputs a b\n.outputs y\n.subckt $reduce_or A[0]=a A[1]=b Y=y\n" \
               f".param A_WIDTH {ten}\n.end\n"
        for stem in ("in", "a_clean", "a_rmdead", "a_prune", "a_arst"):
            self.set_blif(stem, body)
        self.set_blif("a", body.replace("A[0]=a A[1]=b", "A[0]=a A[1]=a"))
        code, out, _ = self.run_tool("--bisect")
        self.assertEqual(code, 1)
        self.assertIn("t.v: differs: no phase differs", out)
        work = self.keep / "t"
        self.assertIn(f".param A_WIDTH {ten}", (work / "b.blif").read_text())
        self.assertIn(".param A_WIDTH 10\n", (work / "b.norm.blif").read_text())
        # A partial result whose width really is 2 is a difference, not a match with b's copy.
        self.set_blif("a_prune", body.replace(ten, "0" * 30 + "10"))
        code, out, _ = self.run_tool("--bisect")
        self.assertIn("t.v: differs: first difference after prune", out)

    def test_normalising_twice_is_safe(self) -> None:
        path = self.dir / "x.blif"
        path.write_text(".param WIDTH " + "0" * 28 + "1010\n", encoding="utf-8")
        ints = proc_diff.load_int_params()
        for _ in range(3):
            self.assertEqual(proc_diff.normalise_file(path, ints).read_text(),
                             ".param WIDTH 10\n")

    def test_hard_block_model_is_compared(self) -> None:
        """I2: with -blackbox Yosys writes the hard block's interface, so it compares."""
        text = (".model top\n.inputs a clk\n.outputs y\n"
                ".subckt single_port_ram addr=a clk=clk out=y\n.end\n"
                ".model single_port_ram\n.inputs addr clk\n.outputs out\n.blackbox\n.end\n")
        self.set_blif("default", text)
        self.assertEqual(self.run_tool()[0], 0)
        self.assertTrue(all("write_blif -blackbox -icells -param" in s
                            for s in self.scripts()[1:]))

    def test_constant_cover_is_not_a_buffer(self) -> None:
        """M1: `.names a y` with rows `1 1`, `0 1` is constant 1, not a buffer of a."""
        text = ".model top\n.names a y\n1 1\n0 1\n.names y z\n1 1\n.end\n"
        self.assertEqual(proc_diff.normalise_text(text, frozenset()), text)

    def test_buffer_roots_are_per_model(self) -> None:
        """M2: a net name reused in another model is another net."""
        text = (".model top\n.names a b\n1 1\n.names b c\n1 1\n.end\n"
                ".model sub\n.names x a\n1 1\n.names a c\n1 1\n.end\n")
        self.assertEqual(proc_diff.normalise_text(text, frozenset()), (
            ".model top\n.names a b\n1 1\n.names a c\n1 1\n.end\n"
            ".model sub\n.names x a\n1 1\n.names x c\n1 1\n.end\n"))

    def test_buffer_cycle_stays(self) -> None:
        """M4: a cycle of two buffers is not two self-loops."""
        text = ".model top\n.names y1 y2\n1 1\n.names y2 y1\n1 1\n.names y2 z\n1 1\n.end\n"
        self.assertEqual(proc_diff.normalise_text(text, frozenset()), text)
        head = ".model top\n.inputs a\n.outputs y1 y2\n"
        self.set_blif("in", head + ".names y1 y2\n1 1\n.names y2 y1\n1 1\n.end\n")
        self.set_blif("a", head + ".names y1 y1\n1 1\n.names y2 y2\n1 1\n.end\n")
        self.assertEqual(self.run_tool()[0], 1)

    def test_assert_and_e3_only_on_their_side(self) -> None:
        """M3: the #17 assert counts on side B only, E3 on side A only."""
        assert17 = "ERROR: Assert `ctrl_sig.size() == 1' failed in passes/proc/proc_mux.cc:264."
        code, out, _ = self.run_tool(env={"FAKE_YOSYS_FAIL": "a", "FAKE_YOSYS_MSG": assert17})
        self.assertEqual(code, 2)
        self.assertIn("t.v: error: yosys exited 1 (step A-BLIF)", out)
        cite16 = self.expect("t.v §20 #16 chain\n")
        e3 = "error: asynchronous reset 'r' does not settle the value of 'q' after 100"
        code, out, _ = self.run_tool("--expect-fail", cite16,
                                     env={"FAKE_YOSYS_FAIL": "in", "FAKE_YOSYS_MSG": e3})
        self.assertEqual(code, 1)
        self.assertIn("listed as §20 #16 (odin3-e3), but error", out)

    def test_17_still_builds_a_blif(self) -> None:
        """Side A's BLIF is still made from a.il after Yosys asserts on side B."""
        env = {"FAKE_YOSYS_FAIL": "in", "FAKE_YOSYS_MSG": "Assert `ctrl_sig.size() == 1'"}
        self.assertEqual(self.run_tool(env=env)[0], 0)
        self.assertEqual(len(self.scripts()), 3)  # A, B, A-BLIF; nothing to compare
        self.assertTrue(self.scripts()[-1].startswith(self.sub("read_rtlil {W}/a.il")))
        self.assertEqual(len(self.argvs()), 4)  # -V, A, B, A-BLIF: no netlist-compare

    def test_unexpected_exception_exits_2(self) -> None:
        """M5: a 2D module of another shape is an error, not a difference."""
        sweep = self.dir / "elab_sweep.py"
        sweep.write_text("def collect(paths):\n    raise TypeError('other shape')\n",
                         encoding="utf-8")
        with mock.patch.object(proc_diff, "ELAB_SWEEP_PY", sweep):
            code, _, err = self.run_tool()
        self.assertEqual(code, 2)
        self.assertIn("internal error: TypeError: other shape", err)

    def test_name_collisions_and_stale_entries(self) -> None:
        """M8: two t.v get two work directories; shared names and stale entries are warned."""
        other = self.dir / "other"
        other.mkdir()
        (other / "t.v").write_text("module top; endmodule // other\n", encoding="utf-8")
        listed = self.expect("nope.v typo\n")
        code, _, err = self.run_tool("--expect-fail", listed,
                                     inputs=(str(self.rows), str(other)))
        self.assertEqual(code, 0)
        self.assertTrue((self.keep / "t").is_dir())
        self.assertTrue((self.keep / "t-2").is_dir())
        self.assertIn("warning: several files are named 't.v'", err)
        self.assertIn("warning: expect-fail entry 'nope.v' matches no file", err)

    def test_memcap_capture_keyword(self) -> None:
        """M9: capture_output=True is passed when memcap.run takes it (the #17 test needs the
        output), and a memcap.run(argv, cap) that always captures is called with two."""
        self.memcap.write_text(
            FAKE_MEMCAP.replace(", capture_output=False", "")
            .replace("capture_output=capture_output", "capture_output=True"), encoding="utf-8")
        env = {"FAKE_YOSYS_FAIL": "in", "FAKE_YOSYS_MSG": "Assert `ctrl_sig.size() == 1'"}
        code, out, _ = self.run_tool(env=env)
        self.assertEqual(code, 0, out)
        self.assertIn("expected difference (§20 #17", out)

    # -- the wrapper -----------------------------------------------------------------------

    def test_wrapper_help(self) -> None:
        wrapper = helpers.REPO_ROOT / "tools" / "proc-check" / "proc-diff"
        result = subprocess.run([str(wrapper), "--help"], capture_output=True, text=True,
                                check=False, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--expect-fail", result.stdout)


if __name__ == "__main__":
    unittest.main()

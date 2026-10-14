"""Tests for tools/vtr-qor/qor_legs.py and tests/vtr/qor/sets.json (3H plan Task 2).

Nothing here runs VTR: commands are built, never executed, and the LUT-size probe runs a fake
Python under /tmp/claude-1000.
"""

from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

import qor_legs
import qor_sched
from qor_heavy import HEAVY_DIR, HeavyError
from qor_legs import LegContext

V = Path("/v")
PY = V / ".venv" / "bin" / "python"
J = Path("/w/j")
ARCH = V / "vtr_flow" / "arch" / "timing" / "k6_frac_N10_frac_chain_mem32K_40nm.xml"
ODIN3 = Path("/o/odin3")
G = Path("/g/k6/vtr/sha/sha.parmys.blif")
R = qor_legs.REPO_ROOT
SHA_V = "/v/vtr_flow/benchmarks/verilog/sha.v"
O_SCRIPT = (
    f"read_techlib {R}/lib/vtr.o3lib; read_verilog --nolatches --vtr-compat {SHA_V}; proc; opt; "
    "flatten; wreduce; hardmap; lower; abc --lut 6; write_blif --vtr --lut-k 6 /w/j/sha.blif"
)
O_PASSES = ("read_techlib", "read_verilog", "proc", "opt", "flatten", "wreduce", "hardmap",
            "lower", "abc", "write_blif")


def ctx(group: str = "A", seed: int = 1, golden: Path | None = None) -> LegContext:
    return LegContext(vtr_root=V, python=PY, arch_xml=ARCH, odin3=ODIN3, repo=R, job_dir=J, k=6,
                      group=group, seed=seed, golden=golden)


class CommandTest(unittest.TestCase):
    def test_p_command(self) -> None:
        (cmd,) = qor_legs.build_commands("P", "sha", ctx())
        self.assertEqual(list(cmd.argv), [
            "/v/.venv/bin/python", "/v/vtr_flow/scripts/run_vtr_flow.py",
            "/v/vtr_flow/benchmarks/verilog/sha.v",
            "/v/vtr_flow/arch/timing/k6_frac_N10_frac_chain_mem32K_40nm.xml",
            "-track_memory_usage", "-crit_path_router_iterations", "100",
            "-temp_dir", "/w/j/run", "-timeout", "7200",
            "-delete_intermediate_files", "-delete_result_files",
        ])
        self.assertEqual(cmd.cwd, J)
        self.assertEqual(cmd.stdout, Path("/w/j/run/vtr_flow.out"))
        env = dict(cmd.env)
        self.assertEqual(list(env), ["PATH"])
        self.assertTrue(env["PATH"].startswith("/v/.venv/bin:"))
        self.assertEqual(env["PATH"], "/v/.venv/bin:" + os.environ.get("PATH", os.defpath))

        (cmd_b,) = qor_legs.build_commands("P", "sha", ctx(group="B"))
        self.assertEqual(cmd_b.argv[cmd_b.argv.index("-timeout") + 1], "14400")

        (cmd_s,) = qor_legs.build_commands("P", "sha", ctx(seed=2))
        i = cmd_s.argv.index("100")
        self.assertEqual(cmd_s.argv[i + 1:i + 3], ("--seed", "2"))
        self.assertEqual(len(cmd_s.argv), len(cmd.argv) + 2)

    def test_odin3_script_texts(self) -> None:
        out = J / "sha.blif"
        o = qor_legs.odin3_script("O", Path(SHA_V), out, 6, "short")
        self.assertEqual(o, O_SCRIPT)
        self.assertEqual(qor_legs.odin3_script("Oprime", Path(SHA_V), out, 6, "short"),
                         O_SCRIPT.replace("abc --lut 6; ", ""))
        oprov = qor_legs.odin3_script("Oprov", Path(SHA_V), out, 6, "provenance")
        self.assertTrue(oprov.endswith("--name-style provenance /w/j/sha.blif"))
        self.assertEqual(oprov, O_SCRIPT.replace(" /w/j/sha.blif",
                                                 " --name-style provenance /w/j/sha.blif"))
        pprime = (f"read_techlib {R}/lib/vtr.o3lib; read_blif {G}; abc --lut 6; "
                  "write_blif --vtr --lut-k 6 /w/j/sha.blif")
        self.assertEqual(qor_legs.odin3_script("Pprime", G, out, 6, "short"), pprime)
        self.assertEqual(qor_legs.odin3_script("R", G, out, 6, "short"),
                         pprime.replace("abc --lut 6; ", ""))
        with self.assertRaises(ValueError) as caught:
            qor_legs.odin3_script("P", Path(SHA_V), out, 6, "short")
        self.assertEqual(str(caught.exception), "leg P runs no odin3 script")
        with self.assertRaises(ValueError):
            qor_legs.odin3_script("O", Path(SHA_V), out, 6, "long")

    def test_frontend_commands(self) -> None:
        first, second = qor_legs.build_commands("O", "sha", ctx())
        self.assertEqual(list(first.argv), [
            "/usr/bin/time", "-v", "-o", "/w/j/frontend.time", str(ODIN3),
            "--diag-paths", "resolved", "-p", O_SCRIPT,
        ])
        self.assertEqual(first.stdout, Path("/w/j/frontend.log"))
        self.assertEqual(first.cwd, J)
        self.assertEqual(first.env, ())
        self.assertEqual(list(second.argv[:6]), [
            str(PY), "/v/vtr_flow/scripts/run_vtr_flow.py", "/w/j/sha.blif", str(ARCH),
            "-start", "vpr",
        ])
        self.assertEqual(list(second.argv[6:]), qor_legs.common_params(J, "A"))
        self.assertEqual(second.stdout, Path("/w/j/run/vtr_flow.out"))
        self.assertEqual(second.cwd, J)

        _, oprime = qor_legs.build_commands("O'", "sha", ctx())
        self.assertEqual(oprime.argv[4:6], ("-start", "abc"))
        for leg, start in (("Oprov", "vpr"), ("Pprime", "vpr"), ("R", "abc")):
            front, vtr = qor_legs.build_commands(leg, "sha", ctx(golden=G))
            self.assertEqual(vtr.argv[4:6], ("-start", start), leg)
            expected = qor_legs.odin3_script(
                leg, G if leg in ("Pprime", "R") else Path(SHA_V), J / "sha.blif", 6,
                "provenance" if leg == "Oprov" else "short")
            self.assertEqual(front.argv[-1], expected, leg)
        with self.assertRaises(ValueError):
            qor_legs.build_commands("Pprime", "sha", ctx())

    def test_key_params(self) -> None:
        self.assertEqual(qor_legs.key_params(),
                         ["-track_memory_usage", "-crit_path_router_iterations", "100"])
        self.assertEqual(qor_legs.key_params(3)[-2:], ["--seed", "3"])


class LegTest(unittest.TestCase):
    def test_leg_names(self) -> None:
        self.assertEqual(qor_legs.normalise_leg("O'"), "Oprime")
        self.assertEqual(qor_legs.normalise_leg("P'"), "Pprime")
        for leg in qor_legs.LEGS:
            self.assertEqual(qor_legs.normalise_leg(leg), leg)
        with self.assertRaises(HeavyError) as caught:
            qor_legs.normalise_leg("X")
        self.assertEqual(caught.exception.code, 2)
        self.assertEqual(caught.exception.message,
                         "unknown leg 'X' (P, O, Oprime, Pprime, R, Oprov)")

    def test_registered_passes(self) -> None:
        text = ("usage: odin3 [options]\n"
                "  --help           print this help\n"
                "passes:\n"
                "  read_blif <file>: read a BLIF netlist\n"
                "  check: check the IR invariants\n"
                "  --top NAME the top module\n"
                "                   name files in diagnostics\n")
        self.assertEqual(qor_legs.registered_passes(text), frozenset({"read_blif", "check"}))

    def test_leg_available(self) -> None:
        passes = frozenset(p for p in O_PASSES if p != "wreduce")
        self.assertEqual(qor_legs.leg_available("O", passes), "wreduce")
        self.assertIsNone(qor_legs.leg_available("O", frozenset(O_PASSES)))
        self.assertIsNone(qor_legs.leg_available("P", frozenset()))
        self.assertEqual(qor_legs.leg_available("Pprime", frozenset(O_PASSES)), "read_blif")
        self.assertEqual(qor_legs.LEG_PASSES["O"], O_PASSES)
        self.assertEqual(qor_legs.LEG_PASSES["Oprov"], O_PASSES)
        self.assertNotIn("abc", qor_legs.LEG_PASSES["Oprime"])

    def test_lut_size(self) -> None:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR, prefix="qor-legs-test-") as name:
            tmp = Path(name)
            record = tmp / "argv"
            fake = tmp / "python"
            fake.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" > {record}\necho ${{FAKE_K:-6}}\n')
            fake.chmod(0o755)
            self.assertEqual(qor_legs.lut_size(V, fake, ARCH), 6)
            argv = record.read_text().splitlines()
            self.assertEqual(argv[:3], ["-I", "-B", "-c"])
            self.assertEqual(argv[-1], str(ARCH))
            self.assertIn("sys.path.insert(0, '/v/vtr_flow/scripts/python_libs')",
                          "\n".join(argv))
            self.assertIn("determine_lut_size", "\n".join(argv))
            fake.write_text("#!/bin/sh\necho boom >&2\nexit 1\n")
            with self.assertRaises(HeavyError) as caught:
                qor_legs.lut_size(V, fake, ARCH)
            self.assertEqual(caught.exception.message,
                             f"cannot read the LUT size of {ARCH}: boom")


class SetsTest(unittest.TestCase):
    def test_sets_json(self) -> None:
        sets = qor_sched.load_sets(R / "tests" / "vtr" / "qor" / "sets.json")
        self.assertEqual(len(sets.order), 21)
        sizes = [sets.designs[d].golden_bytes for d in sets.order]
        self.assertEqual(sizes, sorted(sizes))
        self.assertEqual(len(set(sizes)), 21)
        self.assertEqual(len(sets.gate), 19)
        self.assertEqual(sets.extra, ("spree", "arm_core"))
        self.assertEqual(sets.smoke, sets.gate[:5])
        self.assertEqual(sets.group_b, ("LU8PEEng", "bgm", "mcml", "LU32PEEng"))
        self.assertEqual(sets.designs["mcml"].vtr_flow_time_s, 4699.4)
        self.assertEqual(sets.designs["ch_intrinsics"].golden_bytes, 59473)
        self.assertEqual([sets.designs[d].rank for d in sets.gate], list(range(1, 20)))
        self.assertIsNone(sets.designs["spree"].rank)
        self.assertEqual(sets.order.index("spree"), sets.order.index("mkPktMerge") + 1)
        self.assertEqual(sets.order.index("arm_core"), sets.order.index("stereovision1") + 1)
        self.assertEqual(sets.order[-4:], sets.group_b)
        self.assertEqual({d for d, i in sets.designs.items() if i.group == "B"},
                         set(sets.group_b))


if __name__ == "__main__":
    unittest.main()

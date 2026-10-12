"""Tests for tools/vtr-qor/qor_sched.py, the planning half (3H plan Task 2: T5 and the planner).

Every case builds a fake VTR tree, repo copy, golden tree, odin3 and ldd under /tmp/claude-1000
and reaches them through module seams (mock.patch.object), never through the environment.
Nothing runs VTR.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest
from collections.abc import Callable
from contextlib import ExitStack
from pathlib import Path
from typing import Any
from unittest import mock

import qor_legs
import qor_sched
from qor_heavy import HEAVY_DIR, HeavyError
from qor_sched import KeyInputs, PlanRequest

PIN = qor_legs.VTR_PIN
K6 = "k6_frac_N10_frac_chain_mem32K_40nm"
SETS = qor_sched.load_sets(qor_legs.REPO_ROOT / "tests" / "vtr" / "qor" / "sets.json")
HEX = frozenset("0123456789abcdef")

BASE = KeyInputs(
    leg="O", design="sha", arch_base=K6, arch_sha256="a" * 64, source_sha256="b" * 64,
    vtr_commit=PIN, params=tuple(qor_legs.key_params()), odin3_build="c" * 64,
    script="read_techlib $LIB; ...", lib_sha256="d" * 64, abc_script_sha256="e" * 64,
)


def _changed(value: Any) -> Any:
    if value is None:
        return "x"
    if isinstance(value, tuple):
        return (*value, "x")
    return value + "x"


def _bump(path: Path) -> None:
    """Change one byte of a file."""
    data = bytearray(path.read_bytes())
    data[0] ^= 1
    path.write_bytes(bytes(data))


class KeyTest(unittest.TestCase):
    def test_job_key_inputs(self) -> None:
        key = qor_sched.job_key(BASE)
        self.assertEqual(len(key), 64)
        self.assertTrue(set(key) <= HEX)
        self.assertEqual(len(qor_sched.leg_key(BASE)), 64)
        seen = {key}
        for field in dataclasses.fields(KeyInputs):
            changed = dataclasses.replace(BASE, **{field.name: _changed(getattr(BASE, field.name))})
            other = qor_sched.job_key(changed)
            self.assertNotEqual(other, key, field.name)
            seen.add(other)
        self.assertEqual(len(seen), len(dataclasses.fields(KeyInputs)) + 1)
        # The canonical JSON: sorted keys, no blanks, schema 1.
        canon = dict(dataclasses.asdict(BASE), schema=1)
        text = json.dumps(canon, sort_keys=True, separators=(",", ":"))
        self.assertEqual(key, hashlib.sha256(text.encode()).hexdigest())

    def test_leg_key_shares_designs(self) -> None:
        other = dataclasses.replace(BASE, design="ch_intrinsics", source_sha256="f" * 64)
        self.assertEqual(qor_sched.leg_key(BASE), qor_sched.leg_key(other))
        self.assertNotEqual(qor_sched.job_key(BASE), qor_sched.job_key(other))

        rebuilt = dataclasses.replace(BASE, odin3_build="9" * 64)
        self.assertNotEqual(qor_sched.job_key(BASE), qor_sched.job_key(rebuilt))
        self.assertNotEqual(qor_sched.leg_key(BASE), qor_sched.leg_key(rebuilt))

    def test_key_excludes_housekeeping(self) -> None:
        params = qor_legs.key_params()
        self.assertNotIn("-temp_dir", params)
        self.assertNotIn("-timeout", params)
        self.assertFalse(any(p.startswith("-delete") for p in params))
        reseeded = dataclasses.replace(BASE, params=tuple(qor_legs.key_params(2)))
        self.assertNotEqual(qor_sched.job_key(BASE), qor_sched.job_key(reseeded))
        self.assertNotEqual(qor_sched.leg_key(BASE), qor_sched.leg_key(reseeded))


class PlanTestBase(unittest.TestCase):
    """A fake VTR tree, repo copy, golden tree, odin3 build and ldd."""

    def setUp(self) -> None:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=HEAVY_DIR, prefix="qor-sched-test-")
        self.addCleanup(self._tmp.cleanup)
        tmp = Path(self._tmp.name)
        self.tmp = tmp
        self.vtr = tmp / "vtr"
        python = self.vtr / ".venv" / "bin" / "python"
        python.parent.mkdir(parents=True)
        python.write_text("#!/bin/sh\necho 6\n")
        python.chmod(0o755)
        timing = self.vtr / "vtr_flow" / "arch" / "timing"
        timing.mkdir(parents=True)
        for arch in (K6, "EArch"):
            (timing / f"{arch}.xml").write_text(f"<architecture name='{arch}'/>\n")
        verilog = self.vtr / "vtr_flow" / "benchmarks" / "verilog"
        verilog.mkdir(parents=True)
        for design in SETS.order:
            (verilog / f"{design}.v").write_text(f"module {design}(); endmodule\n")
        self.repo = tmp / "repo"
        (self.repo / "lib" / "abc").mkdir(parents=True)
        shutil.copyfile(qor_legs.REPO_ROOT / "lib" / "vtr.o3lib", self.repo / "lib" / "vtr.o3lib")
        (self.repo / "lib" / "abc" / "vtr.abc").write_text("strash; dch -f; if -K 6; mfs2\n")
        self.golden = tmp / "golden"
        for design in ("sha", "ch_intrinsics"):
            blif = self.golden / K6 / "vtr" / design / f"{design}.parmys.blif"
            blif.parent.mkdir(parents=True)
            blif.write_text(f".model {design}\n.end\n")
        self.odin3, self.libs = self._build()
        self.out = tmp / "out"
        stack = ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(mock.patch.object(qor_legs, "REPO_ROOT", self.repo))
        stack.enter_context(mock.patch.object(qor_legs, "GOLDEN_ROOT", self.golden))
        stack.enter_context(mock.patch.object(qor_sched, "LDD_CMD", (str(self.ldd),)))
        stack.enter_context(mock.patch.object(qor_legs, "vtr_commit", return_value=PIN))

    def _build(self) -> tuple[Path, dict[str, Path]]:
        build = self.tmp / "x"
        build.mkdir()
        odin3 = build / "odin3"
        odin3.write_bytes(b"\x7fELF odin3")
        libs = {name: build / name for name in ("libodin3.so", "libodin3_abc.so")}
        for name, path in libs.items():
            path.write_bytes(name.encode())
        self.ldd = self.tmp / "ldd"
        lines = "".join(f"\t{n} => {p} (0x00007f0000000000)\\n" for n, p in libs.items())
        self.ldd.write_text(
            "#!/bin/sh\n"
            f'printf "\\tlinux-vdso.so.1 (0x00007ffd00000000)\\n{lines}'
            '\\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f0000000000)\\n"\n'
        )
        self.ldd.chmod(0o755)
        return odin3, libs

    def request(self, legs: tuple[str, ...], designs: tuple[str, ...], *, rerun: bool = False,
                arch: str = "k6") -> PlanRequest:
        return PlanRequest(legs=legs, designs=designs, arch=arch, out=self.out,
                           vtr_root=self.vtr, odin3=self.odin3, rerun=rerun)

    def keys(self, legs: tuple[str, ...]) -> dict[tuple[str, str], str]:
        jobs = qor_sched.plan_jobs(self.request(legs, ("sha", "ch_intrinsics")), SETS)
        return {(job.leg, job.design): job.key for job in jobs}


class BuildIdTest(PlanTestBase):
    def test_build_id(self) -> None:
        first = qor_sched.odin3_build_id(self.odin3)
        self.assertEqual(len(first), 64)
        self.assertEqual(qor_sched.odin3_build_id(self.odin3), first)
        seen = {first}
        for path in (self.libs["libodin3.so"], self.libs["libodin3_abc.so"], self.odin3):
            _bump(path)
            now = qor_sched.odin3_build_id(self.odin3)
            self.assertNotIn(now, seen, path.name)
            seen.add(now)

    def test_build_id_ldd_failures(self) -> None:
        self.ldd.write_text("#!/bin/sh\necho '\tnot a dynamic executable'\nexit 1\n")
        alone = qor_sched.odin3_build_id(self.odin3)
        self.assertEqual(len(alone), 64)
        self.ldd.write_text("#!/bin/sh\necho '\tlibodin3.so.0 => not found'\n")
        with self.assertRaises(HeavyError) as caught:
            qor_sched.odin3_build_id(self.odin3)
        self.assertEqual(caught.exception.message,
                         f"{self.odin3}: {self.ldd} cannot find libodin3.so.0")
        with self.assertRaises(HeavyError) as caught:
            qor_sched.odin3_build_id(self.tmp / "absent")
        self.assertEqual(caught.exception.code, 2)


class PlanTest(PlanTestBase):
    def test_plan_order(self) -> None:
        designs = qor_sched.select_designs(SETS, [], ["sha", "ch_intrinsics"])
        jobs = qor_sched.plan_jobs(self.request(("O", "P"), designs), SETS)
        self.assertEqual([(j.design, j.leg) for j in jobs], [
            ("ch_intrinsics", "P"), ("ch_intrinsics", "O"), ("sha", "P"), ("sha", "O"),
        ])
        # The planner orders by itself, whatever order the request gives.
        unsorted = qor_sched.plan_jobs(self.request(("O", "P"), ("sha", "ch_intrinsics")), SETS)
        self.assertEqual([(j.design, j.leg) for j in unsorted], [(j.design, j.leg) for j in jobs])
        every = qor_sched.plan_jobs(
            self.request(("Oprov", "R", "O", "O'", "P'", "P"), ("sha",)), SETS)
        self.assertEqual([j.leg for j in every], ["P", "Pprime", "Oprime", "O", "R", "Oprov"])

    def test_plan_job_fields(self) -> None:
        jobs = qor_sched.plan_jobs(self.request(("P", "O"), ("sha", "ch_intrinsics")), SETS)
        by = {(j.design, j.leg): j for j in jobs}
        sha_o, ch_o = by[("sha", "O")], by[("ch_intrinsics", "O")]
        self.assertEqual(sha_o.key12, ch_o.key12)          # one leg key per leg configuration
        self.assertNotEqual(sha_o.key, ch_o.key)
        self.assertNotEqual(sha_o.key12, sha_o.key[:12])   # key12 is the leg key's
        self.assertEqual(sha_o.job_dir, self.out / PIN[:12] / K6 / "O" / sha_o.key12 / "sha")
        self.assertEqual((sha_o.arch, sha_o.group, sha_o.gating), (K6, "A", True))
        self.assertEqual(len(sha_o.commands), 2)
        self.assertIn(str(self.repo / "lib" / "vtr.o3lib"), sha_o.commands[0].argv[-1])
        self.assertEqual(sha_o.commands[1].argv[0], str(self.vtr / ".venv" / "bin" / "python"))
        self.assertEqual(len(by[("sha", "P")].commands), 1)
        self.assertFalse(sha_o.cached)

        big = qor_sched.plan_jobs(self.request(("P",), ("mcml", "spree")), SETS)
        self.assertEqual([(j.design, j.group, j.gating) for j in big],
                         [("spree", "A", False), ("mcml", "B", True)])
        self.assertIn("14400", big[1].commands[0].argv)
        earch = qor_sched.plan_jobs(self.request(("P",), ("sha",), arch="earch"), SETS)
        self.assertEqual((earch[0].arch, earch[0].gating), ("EArch", False))
        self.assertEqual(qor_sched.plan_jobs(self.request((), ("sha",)), SETS), [])

    def test_plan_unknown(self) -> None:
        with self.assertRaises(HeavyError) as caught:
            qor_sched.plan_jobs(self.request(("X",), ("sha",)), SETS)
        self.assertEqual(caught.exception.message,
                         "unknown leg 'X' (P, O, Oprime, Pprime, R, Oprov)")
        with self.assertRaises(HeavyError) as caught:
            qor_sched.plan_jobs(self.request(("P",), ("nope",)), SETS)
        self.assertEqual(caught.exception.message,
                         "unknown design 'nope' (see tests/vtr/qor/sets.json)")

    def test_plan_cache(self) -> None:
        (job,) = qor_sched.plan_jobs(self.request(("P",), ("sha",)), SETS)
        metrics = job.job_dir / "metrics.json"
        metrics.parent.mkdir(parents=True)

        def cached(status: str, key: str, rerun: bool = False) -> bool:
            metrics.write_text(json.dumps({"schema": 1, "status": status, "key": key}))
            (again,) = qor_sched.plan_jobs(self.request(("P",), ("sha",), rerun=rerun), SETS)
            return again.cached

        self.assertTrue(cached("ok", job.key))
        self.assertFalse(cached("ok", "0" * 64))
        self.assertFalse(cached("failed", job.key))
        self.assertFalse(cached("ok", job.key, rerun=True))
        metrics.write_text("{not json")
        (again,) = qor_sched.plan_jobs(self.request(("P",), ("sha",)), SETS)
        self.assertFalse(again.cached)

    def test_plan_rekeys_on_lib_and_script(self) -> None:
        legs = ("P", "Oprime", "O", "Pprime")
        before = self.keys(legs)
        for path in (self.repo / "lib" / "vtr.o3lib", self.repo / "lib" / "abc" / "vtr.abc"):
            _bump(path)
            after = self.keys(legs)
            for (leg, design), key in after.items():
                if leg == "P":
                    self.assertEqual(key, before[(leg, design)], (path.name, design))
                else:
                    self.assertNotEqual(key, before[(leg, design)], (path.name, leg, design))
            before = after

    def test_plan_rekeys_on_build_seed_and_golden(self) -> None:
        legs = ("P", "O", "Pprime")
        before = self.keys(legs)
        _bump(self.libs["libodin3_abc.so"])
        after = self.keys(legs)
        for (leg, design), key in after.items():
            self.assertEqual(key == before[(leg, design)], leg == "P", (leg, design))
        _bump(self.golden / K6 / "vtr" / "sha" / "sha.parmys.blif")
        golden = self.keys(legs)
        changed = {lk for lk, key in golden.items() if key != after[lk]}
        self.assertEqual(changed, {("Pprime", "sha")})
        seeded = qor_sched.plan_jobs(dataclasses.replace(
            self.request(legs, ("sha",)), seed=2), SETS)
        self.assertTrue(all(j.key != golden[(j.leg, j.design)] for j in seeded))
        self.assertIn("--seed", seeded[0].commands[0].argv)

    def test_plan_missing_golden(self) -> None:
        (job,) = qor_sched.plan_jobs(self.request(("R",), ("diffeq1",)), SETS)
        self.assertIn(str(qor_legs.golden_blif(K6, "diffeq1")), job.commands[0].argv[-1])
        self.assertEqual(job.commands[1].argv[4:6], ("-start", "abc"))


class SelectTest(unittest.TestCase):
    def test_select_designs(self) -> None:
        self.assertEqual(qor_sched.select_designs(SETS, ["gate"], []), SETS.gate)
        self.assertEqual(qor_sched.select_designs(SETS, ["extra", "gate"], []), SETS.order)
        self.assertEqual(qor_sched.select_designs(SETS, ["smoke"], ["mcml", "spree"]),
                         (*SETS.smoke, "spree", "mcml"))
        with self.assertRaises(HeavyError) as caught:
            qor_sched.select_designs(SETS, ["big"], [])
        self.assertEqual(caught.exception.message, "unknown set 'big' (gate, extra, smoke)")
        with self.assertRaises(HeavyError) as caught:
            qor_sched.select_designs(SETS, [], ["nope"])
        self.assertEqual((caught.exception.code, caught.exception.message),
                         (2, "unknown design 'nope' (see tests/vtr/qor/sets.json)"))

    def test_load_sets_refusals(self) -> None:
        good = json.loads((qor_legs.REPO_ROOT / "tests/vtr/qor/sets.json").read_text())
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR, prefix="qor-sets-test-") as name:
            path = Path(name) / "sets.json"
            mutations: list[Callable[[dict[str, Any]], object]] = [
                lambda d: d.update(schema=2),
                lambda d: d["order"].reverse(),
                lambda d: d["smoke"].append("spree"),
                lambda d: d["group_b"].pop(),
                lambda d: d["designs"]["sha"].update(group="C"),
                lambda d: d["gate"].append("spree"),
                lambda d: d["designs"]["sha"].update(golden_bytes="big"),
                lambda d: d.pop("designs"),
            ]
            for mutate in mutations:
                data = json.loads(json.dumps(good))
                mutate(data)
                path.write_text(json.dumps(data))
                with self.assertRaises(HeavyError) as caught:
                    qor_sched.load_sets(path)
                self.assertEqual(caught.exception.code, 2)
                self.assertTrue(caught.exception.message.startswith(f"{path}: "))


class VtrCommitTest(unittest.TestCase):
    def test_vtr_commit(self) -> None:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=HEAVY_DIR, prefix="qor-git-test-") as name:
            root = Path(name)
            with self.assertRaises(HeavyError) as caught:
                qor_legs.vtr_commit(root)
            self.assertEqual(caught.exception.message,
                             f"VTR not found at {root} (set VTR_ROOT or --vtr-root)")
            git = ["git", "-C", name, "-c", "user.name=t", "-c", "user.email=t@t",
                   "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false"]
            subprocess.run([*git, "init", "-q"], check=True)
            subprocess.run([*git, "commit", "-q", "--allow-empty", "-m", "x"], check=True)
            head = subprocess.run([*git, "rev-parse", "HEAD"], check=True, capture_output=True,
                                  text=True).stdout.strip()
            self.assertEqual(qor_legs.vtr_commit(root), head)


if __name__ == "__main__":
    unittest.main()

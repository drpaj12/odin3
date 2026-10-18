"""Tests for tools/vtr-qor/qor_sched.py: the planner (3H plan Task 2: T5) and the runner (Task 5:
T4).

Every case builds its fakes (a VTR tree, repo copy, golden tree, odin3 and ldd; a cgroup tree,
descriptor 9, df, meminfo and scripted fake jobs) under /tmp/claude-1000 and reaches them through
module seams (mock.patch.object), never through an environment hook. Nothing runs VTR, takes the
real heavy lock or reads the real cgroups.
"""

from __future__ import annotations

import dataclasses
import functools
import gzip
import hashlib
import io
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from collections.abc import Callable
from contextlib import ExitStack, redirect_stderr
from pathlib import Path
from typing import Any
from unittest import mock

import qor_heavy
import qor_legs
import qor_parse
import qor_sched
from qor_heavy import HEAVY_DIR, HeavyError
from qor_sched import Job, KeyInputs, PlanRequest

from .helpers import write_golden

PIN = qor_legs.VTR_PIN
K6 = "k6_frac_N10_frac_chain_mem32K_40nm"
SETS = qor_sched.load_sets(qor_legs.REPO_ROOT / "tests" / "vtr" / "qor" / "sets.json")
HEX = frozenset("0123456789abcdef")
ALL_LEGS = ("P", "Pprime", "Oprime", "O", "R", "Oprov")

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

    def test_leg_key_inputs(self) -> None:
        """The leg key (and so key12) changes with every input but the design and its source."""
        key = qor_sched.leg_key(BASE)
        for field in dataclasses.fields(KeyInputs):
            changed = dataclasses.replace(BASE, **{field.name: _changed(getattr(BASE, field.name))})
            same = qor_sched.leg_key(changed) == key
            self.assertEqual(same, field.name in ("design", "source_sha256"), field.name)
            self.assertEqual(qor_sched.leg_key(changed)[:12] == key[:12], same, field.name)

    def test_max_jobs(self) -> None:
        self.assertEqual(qor_sched.MAX_JOBS, 1)  # ruling C1, point (w): change it deliberately

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
            write_golden(self.golden / K6, design, f".model {design}\n.end\n")
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
        # Out of basename order, with libc between the two (point (q): only libodin3*.so count).
        abc, core = libs["libodin3_abc.so"], libs["libodin3.so"]
        self.libc = build / "libc.so.6"
        self.libc.write_bytes(b"libc")
        self.ldd.write_text(
            "#!/bin/sh\n"
            'printf "\\tlinux-vdso.so.1 (0x00007ffd00000000)\\n"\n'
            f'printf "\\tlibodin3_abc.so => {abc} (0x00007f0000000000)\\n"\n'
            f'printf "\\tlibc.so.6 => {self.libc} (0x00007f0000000000)\\n"\n'
            f'printf "\\tlibodin3.so => {core} (0x00007f0000000000)\\n"\n'
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

    def patching(self, name: str, **kwargs: Any) -> Callable[[], object]:
        """A mutation that patches ``qor_legs.<name>`` for the rest of the test."""
        def start() -> object:
            patcher = mock.patch.object(qor_legs, name, **kwargs)
            self.addCleanup(patcher.stop)
            return patcher.start()
        return start

    def plan_all(self) -> list[Job]:
        return qor_sched.plan_jobs(self.request(ALL_LEGS, ("sha", "ch_intrinsics")), SETS)

    def assert_rekeys(self, mutate: Callable[[], object], affected: Callable[[Job], bool],
                      *, key12_moves: bool) -> None:
        """Plan every leg on two designs and mark each job ``ok`` under its key; after
        ``mutate``, exactly the ``affected`` jobs re-key (their key12 moving iff
        ``key12_moves``) and are not cached, and every other job is reused unchanged."""
        before = self.plan_all()
        for job in before:
            job.job_dir.mkdir(parents=True, exist_ok=True)
            (job.job_dir / "metrics.json").write_text(
                json.dumps({"schema": 1, "status": "ok", "key": job.key}))
        self.assertTrue(all(j.cached for j in self.plan_all()))
        mutate()
        after = self.plan_all()
        self.assertEqual([(j.design, j.leg) for j in after], [(j.design, j.leg) for j in before])
        for old, new in zip(before, after, strict=True):
            where = (new.leg, new.design)
            if affected(new):
                self.assertNotEqual(new.key, old.key, where)
                self.assertFalse(new.cached, where)
                self.assertEqual(new.key12 != old.key12, key12_moves, where)
            else:
                self.assertEqual((new.key, new.key12, new.cached), (old.key, old.key12, True),
                                 where)


class BuildIdTest(PlanTestBase):
    def test_build_id(self) -> None:
        first = qor_sched.odin3_build_id(self.odin3)
        self.assertEqual(len(first), 64)
        manifest = "".join(
            f"{name}\0{hashlib.sha256(path.read_bytes()).hexdigest()}\n"
            for name, path in (("odin3", self.odin3), ("libodin3.so", self.libs["libodin3.so"]),
                               ("libodin3_abc.so", self.libs["libodin3_abc.so"])))
        self.assertEqual(first, hashlib.sha256(manifest.encode()).hexdigest())
        _bump(self.libc)
        self.assertEqual(qor_sched.odin3_build_id(self.odin3), first)  # libc is not odin3's
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
        everything = qor_sched.plan_jobs(self.request(("P",), tuple(reversed(SETS.order))), SETS)
        self.assertEqual(tuple(j.design for j in everything), SETS.order)
        self.assertEqual(tuple(j.group for j in everything), ("A",) * 17 + ("B",) * 4)
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

    def test_plan_key_inputs(self) -> None:
        """Each planned key is §5.3's over the real inputs (point (f)), hand-built here."""
        sha = hashlib.sha256
        jobs = {(j.leg, j.design): j for j in self.plan_all()}
        xml = self.vtr / "vtr_flow/arch/timing" / f"{K6}.xml"
        v_sha = sha((self.vtr / "vtr_flow/benchmarks/verilog/sha.v").read_bytes()).hexdigest()
        g_sha = sha((self.golden / K6 / "vtr/sha/sha.parmys.blif").read_bytes()).hexdigest()
        p = KeyInputs(
            leg="P", design="sha", arch_base=K6, arch_sha256=sha(xml.read_bytes()).hexdigest(),
            source_sha256=v_sha, vtr_commit=PIN, params=tuple(qor_legs.key_params()),
            odin3_build=None, script=None, lib_sha256=None, abc_script_sha256=None,
        )
        o = dataclasses.replace(
            p, leg="O", odin3_build=qor_sched.odin3_build_id(self.odin3),
            lib_sha256=sha((self.repo / "lib/vtr.o3lib").read_bytes()).hexdigest(),
            abc_script_sha256=sha((self.repo / "lib/abc/vtr.abc").read_bytes()).hexdigest(),
            script="read_techlib $LIB; read_verilog --nolatches --vtr-compat $IN; proc; opt; "
            "flatten; wreduce; hardmap; lower; abc --lut 6; write_blif --vtr --lut-k 6 $OUT",
        )
        pprime = dataclasses.replace(
            o, leg="Pprime", source_sha256=g_sha,
            script="read_techlib $LIB; read_blif $IN; abc --lut 6; write_blif --vtr --lut-k 6 $OUT",
        )
        expected = {"P": p, "O": o, "Pprime": pprime}
        for leg, inputs in expected.items():
            job = jobs[(leg, "sha")]
            self.assertEqual(job.key, qor_sched.job_key(inputs), leg)
            self.assertEqual(job.key12, qor_sched.leg_key(inputs)[:12], leg)

    def test_plan_rekeys_on_lib_and_script(self) -> None:
        def odin3_legs(job: Job) -> bool:
            return job.leg != "P"

        for path in (self.repo / "lib" / "vtr.o3lib", self.repo / "lib" / "abc" / "vtr.abc"):
            self.assert_rekeys(functools.partial(_bump, path), odin3_legs, key12_moves=True)
        # The script text itself (with K in it).
        key_script = qor_legs.key_script
        self.assert_rekeys(self.patching("key_script",
                                         side_effect=lambda *a: key_script(*a) + "; stats"),
                           odin3_legs, key12_moves=True)
        self.assert_rekeys(self.patching("lut_size", return_value=4), odin3_legs,
                           key12_moves=True)

    def test_plan_rekeys_on_sources(self) -> None:
        """A design's source re-keys that design's jobs alone, in the same key12 directory."""
        v = self.vtr / "vtr_flow" / "benchmarks" / "verilog" / "sha.v"
        self.assert_rekeys(lambda: _bump(v),
                           lambda job: job.design == "sha" and job.leg not in ("Pprime", "R"),
                           key12_moves=False)
        g = self.golden / K6 / "vtr" / "sha" / "sha.parmys.blif"
        self.assert_rekeys(lambda: _bump(g),
                           lambda job: job.design == "sha" and job.leg in ("Pprime", "R"),
                           key12_moves=False)

    def test_plan_rekeys_on_arch_and_vtr(self) -> None:
        xml = self.vtr / "vtr_flow" / "arch" / "timing" / f"{K6}.xml"
        self.assert_rekeys(lambda: _bump(xml), lambda job: True, key12_moves=True)
        self.assert_rekeys(self.patching("vtr_commit", return_value="f" * 40), lambda job: True,
                           key12_moves=True)
        moved = self.plan_all()[0]
        self.assertEqual(moved.job_dir.relative_to(self.out).parts[0], "f" * 12)

    def test_plan_rekeys_on_build_seed_and_golden(self) -> None:
        legs = ("P", "O", "Pprime")
        before = self.keys(legs)
        _bump(self.libs["libodin3_abc.so"])
        after = self.keys(legs)
        for (leg, design), key in after.items():
            self.assertEqual(key == before[(leg, design)], leg == "P", (leg, design))
        write_golden(self.golden / K6, "sha", ".model sha\n# regenerated\n.end\n")
        golden = self.keys(legs)
        changed = {lk for lk, key in golden.items() if key != after[lk]}
        self.assertEqual(changed, {("Pprime", "sha")})
        self.assert_rekeys(lambda: _bump(self.libs["libodin3.so"]), lambda job: job.leg != "P",
                           key12_moves=True)
        seeded = qor_sched.plan_jobs(dataclasses.replace(
            self.request(legs, ("sha",)), seed=2), SETS)
        self.assertTrue(all(j.key != golden[(j.leg, j.design)] for j in seeded))
        self.assertIn("--seed", seeded[0].commands[0].argv)

    def test_plan_missing_golden(self) -> None:
        (job,) = qor_sched.plan_jobs(self.request(("R",), ("diffeq1",)), SETS)
        self.assertIn(str(qor_legs.golden_blif(K6, "diffeq1")), job.commands[0].argv[-1])
        (with_golden,) = qor_sched.plan_jobs(self.request(("R",), ("sha",)), SETS)
        inputs = KeyInputs(
            leg="R", design="diffeq1", arch_base=K6,
            arch_sha256=qor_sched.file_sha256(self.vtr / "vtr_flow/arch/timing" / f"{K6}.xml"),
            source_sha256=qor_sched.GOLDEN_MISSING, vtr_commit=PIN,
            params=tuple(qor_legs.key_params()), odin3_build=qor_sched.odin3_build_id(self.odin3),
            script="read_techlib $LIB; read_blif $IN; write_blif --vtr --lut-k 6 $OUT",
            lib_sha256=qor_sched.file_sha256(self.repo / "lib/vtr.o3lib"),
            abc_script_sha256=qor_sched.file_sha256(self.repo / "lib/abc/vtr.abc"),
        )
        self.assertEqual(qor_sched.GOLDEN_MISSING, "missing")
        self.assertEqual(job.key, qor_sched.job_key(inputs))
        self.assertEqual(job.key12, with_golden.key12)
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
                lambda d: d.update(schema=True),
                lambda d: d.update(schema="1"),
                lambda d: d["designs"]["sha"].update(vtr_flow_time_s=float("nan")),
                lambda d: d["designs"]["sha"].update(vtr_flow_time_s=float("inf")),
                lambda d: d["designs"]["sha"].update(vtr_flow_time_s=-1.0),
                lambda d: d["designs"]["sha"].update(vtr_flow_time_s=0),
                lambda d: d["designs"]["sha"].update(vtr_flow_time_s=True),
                lambda d: d["designs"]["ch_intrinsics"].update(golden_bytes=0),
                lambda d: d["designs"]["ch_intrinsics"].update(golden_bytes=-1),
                lambda d: d["designs"]["ch_intrinsics"].update(golden_bytes=1.5),
                lambda d: d["designs"]["sha"].update(golden_bytes=1),  # out of size order
                lambda d: d["designs"]["sha"].update(rank=0),
                lambda d: d["designs"]["sha"].update(rank=True),
                lambda d: [d["designs"][n].update(rank=1) for n in d["gate"]],
                lambda d: (d["designs"]["sha"].update(rank=6),
                           d["designs"]["raygentop"].update(rank=7)),
                lambda d: d["designs"]["spree"].update(rank=20),
                lambda d: d["designs"]["sha"].update(extra=1),
                lambda d: d["designs"]["sha"].pop("rank"),
                lambda d: d["designs"].update(sha=[1]),
                lambda d: d.update(smoke=[]),
                lambda d: d.update(smoke=d["gate"][1:6]),
                lambda d: d.update(order=[], gate=[], extra=[], smoke=[], group_b=[], designs={}),
                lambda d: d.update(gate="sha"),
                lambda d: d["extra"].append("spree"),
                lambda d: d.update(extra=["arm_core", "spree"]),
                lambda d: d.update(group_b=["bgm", "LU8PEEng", "mcml", "LU32PEEng"]),
                lambda d: d["designs"]["blob_merge"].update(group="B"),
                lambda d: d["designs"]["mcml"].update(group="A"),
                lambda d: d["order"].reverse(),
                lambda d: d["smoke"].append("spree"),
                lambda d: d["group_b"].pop(),
                lambda d: d["designs"]["sha"].update(group="C"),
                lambda d: d["gate"].append("spree"),
                lambda d: d["designs"]["sha"].update(golden_bytes="big"),
                lambda d: d.pop("designs"),
            ]
            texts = ["", "[]", "{}"]
            for mutate in mutations:
                data = json.loads(json.dumps(good))
                mutate(data)
                texts.append(json.dumps(data))
            # A design name that would escape <J> (renamed everywhere it appears).
            texts.append(json.dumps(good).replace('"sha"', '"../sha"'))
            for i, text in enumerate(texts):
                path.write_text(text)
                with self.assertRaises(HeavyError, msg=f"case {i}") as caught:
                    qor_sched.load_sets(path)
                self.assertEqual(caught.exception.code, 2)
                self.assertTrue(caught.exception.message.startswith(f"{path}: "))
            path.write_text(json.dumps(good))
            self.assertEqual(qor_sched.load_sets(path), SETS)


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



# -- the runner (plan Task 5: T4) --------------------------------------------------------------

FAKEJOB = Path(__file__).resolve().parent / "fixtures" / "vtr_qor" / "fakejob" / "fakejob.py"
GIB = 1024**3
MIB = 1024**2
KIB_PER_GIB = 1024**2
VTR12 = PIN[:12]
KEY12 = "0123456789ab"
START_LINE = re.compile(r"vtr-qor: \[\d+/\d+\] \S+ \S+ start")
SLICE_DIRS = "user.slice/user-1000.slice/user@1000.service/odin3.slice"
HINT = ("vtr-qor: free C: by compacting the WSL disk (wsl --shutdown, then Optimize-VHD or "
        "diskpart compact vdisk; Peter's hands)")
KEEP_PRESENT = ("vtr_flow.out", "output.txt", "parse_results.txt", "qor_results.txt", "vpr.out",
                "vpr.crit_path.out", "parmys.out", "abc0.out", "abc1.out",
                "sha_blackboxing_latch.out", "restore_latch0.out", "report_clocks.abc.out",
                "parse_results.err", "qor_results.err")  # the parse's stderr (final review M1)
SCRATCH = ("sha.pre-vpr.blif", "sha.net", "sha.place", "sha.route", "sha.parmys.blif",
           "sha.abc.blif")
PARSE_TEXT = "arch\tcircuit\tvpr_status\t\nk6.xml\tsha.v\tsuccess\t\n"

Spec = dict[str, Any]


class FakeClock:
    """A clock that moves only when told: ``tick`` per call, and by the slept time."""

    def __init__(self, tick: float = 0.0) -> None:
        self.now = 1_000_000.0
        self.tick = tick
        self.sleeps: list[float] = []
        self.on_sleep: Callable[[int], None] | None = None

    def __call__(self) -> float:
        self.now += self.tick
        return self.now

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        self.now += seconds
        if self.on_sleep is not None:
            self.on_sleep(len(self.sleeps))


class ScaledClock:
    """Real time sped up ``scale`` times, so POLL_S passes in a fraction of a second."""

    def __init__(self, scale: float) -> None:
        self.t0 = time.monotonic()
        self.scale = scale

    def __call__(self) -> float:
        return 1_000_000.0 + (time.monotonic() - self.t0) * self.scale


def _alive(pid: int) -> bool:
    """True while ``pid`` runs (a zombie or a missing process is not alive)."""
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return False
    return stat.rsplit(")", 1)[1].split()[0] not in ("Z", "X")


def _wait_dead(pids: list[int], limit_s: float = 30.0) -> bool:
    """True once no pid in ``pids`` is alive (it returns as soon as that holds; the default
    limit is generous because a SIGKILLed process can take a while to be scheduled under load)."""
    end = time.monotonic() + limit_s
    while time.monotonic() < end:
        if not any(_alive(pid) for pid in pids):
            return True
        time.sleep(0.02)
    return False


def finish_ok(job: Job, start: float) -> dict[str, object]:
    return {"schema": 1, "leg": job.leg, "routed": {"vpr_status": "success"}}


class RunnerTestBase(unittest.TestCase):
    """A fake heavy run: a cgroup tree with heavy-run's slice and scope, descriptor 9 on a fake
    lock, a fake df on /mnt/c and a fake meminfo, reached only through qor_heavy's module seams
    (and ODIN3_HEAVY_SLICE, which alone proves nothing). Jobs are fakejob.py runs."""

    def setUp(self) -> None:
        HEAVY_DIR.mkdir(parents=True, exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=HEAVY_DIR, prefix="qor-sched-run-")
        self.addCleanup(self._tmp.cleanup)
        self.tmp = Path(self._tmp.name)
        self.out = self.tmp / "out"
        self.out.mkdir()
        self.stamps = self.tmp / "stamps"
        root = self.tmp / "cg"
        self.slice = root / SLICE_DIRS / "odin3-heavy.slice"
        self.leaf = self.slice / "odin3-heavy-4242-1760000000000000000.scope"
        self.leaf.mkdir(parents=True)
        for directory, mem, swap in ((self.leaf, "max", "max"), (self.slice, "6442450944", "0")):
            (directory / "memory.max").write_text(mem + "\n")
            (directory / "memory.swap.max").write_text(swap + "\n")
        self.events = self.slice / "memory.events"
        self.events.write_text("low 0\nhigh 0\nmax 0\noom 0\noom_kill 0\n")
        self_cgroup = self.tmp / "self-cgroup"
        self_cgroup.write_text(f"0::/{self.leaf.relative_to(root)}\n")
        self.lock = self.tmp / "odin3-heavy.lock"
        self.lock.touch()
        fd9 = self.tmp / "fd9"
        fd9.symlink_to(self.lock)
        self.df_file = self.tmp / "df-gib"
        self.set_df(100)
        fake_df = self.tmp / "fake-df.py"
        self.df_hang = self.tmp / "df-hang"
        fake_df.write_text(
            "import os, sys, time\n"
            f"if os.path.exists({str(self.df_hang)!r}):\n"
            "    time.sleep(600)\n"
            "gib = int(open(sys.argv[1]).read())\n"
            f"print('Mounted on   Avail')\nprint('/mnt/c', gib * {GIB})\n")
        self.meminfo = self.tmp / "meminfo"
        self.set_mem(16.0)
        stack = ExitStack()
        self.addCleanup(stack.close)
        for name, value in (("SELF_CGROUP", self_cgroup), ("CGROUP_ROOT", root), ("FD9", fd9),
                            ("HEAVY_LOCK", self.lock),
                            ("DF_CMD", (sys.executable, "-I", str(fake_df), str(self.df_file)))):
            stack.enter_context(mock.patch.object(qor_heavy, name, value))
        stack.enter_context(mock.patch.dict(os.environ, {"ODIN3_HEAVY_SLICE": "odin3-heavy.slice"}))
        self.scope = qor_heavy.require_heavy()
        self.n_specs = 0

    # -- fakes -------------------------------------------------------------------------------

    def set_df(self, gib: int) -> None:
        self.df_file.write_text(f"{gib}\n")

    def set_mem(self, gib: float) -> None:
        kib = int(gib * KIB_PER_GIB)
        self.meminfo.write_text(f"MemTotal: 25000000 kB\nMemAvailable: {kib} kB\n")

    def job_dir(self, design: str, leg: str = "P") -> Path:
        return self.out / VTR12 / K6 / leg / KEY12 / design

    def command(self, spec: Spec, stdout: Path, *, stderr: Path | None = None,
                env: tuple[tuple[str, str], ...] = ()) -> qor_legs.Command:
        self.n_specs += 1
        path = self.tmp / f"spec-{self.n_specs}.json"
        path.write_text(json.dumps(spec))
        return qor_legs.Command((sys.executable, "-I", str(FAKEJOB), str(path)), stdout.parent,
                                stdout, env, stderr)

    def job(self, design: str, specs: list[Spec], *, leg: str = "P", group: str = "A",
            cached: bool = False, key: str = "f" * 64) -> Job:
        """A job whose commands are fake jobs: for an odin3 leg the first is the front end
        (stdout frontend.log), every other one VTR (stdout run/vtr_flow.out)."""
        job_dir = self.job_dir(design, leg)
        commands = []
        for i, spec in enumerate(specs):
            front = leg != "P" and i == 0
            stdout = job_dir / "frontend.log" if front else job_dir / "run" / "vtr_flow.out"
            commands.append(self.command(spec, stdout))
        return Job(leg=leg, design=design, arch=K6, group=group, gating=True, key=key,
                   key12=KEY12, job_dir=job_dir, commands=tuple(commands), cached=cached)

    def opts(self, clock: Callable[[], float] | None = None, **kwargs: Any) -> qor_sched.RunOptions:
        fake = FakeClock()
        kwargs.setdefault("scope", self.scope)
        return qor_sched.RunOptions(
            out=self.out, clock=clock or fake, sleep=kwargs.pop("sleep", fake.sleep),
            meminfo=self.meminfo, **kwargs)

    def run_jobs(self, jobs: list[Job], opts: qor_sched.RunOptions,
                 finish: qor_sched.Finish = finish_ok, *, starts: bool = False
                 ) -> tuple[qor_sched.RunSummary, list[str]]:
        """Run and return the stderr lines; the per-job ``start`` lines (pinned by
        test_run_lines) are left out unless ``starts``."""
        err = io.StringIO()
        with redirect_stderr(err):
            summary = qor_sched.run_jobs(jobs, opts, finish)
        lines = err.getvalue().splitlines()
        if not starts:
            lines = [line for line in lines if not START_LINE.fullmatch(line)]
        return summary, lines

    def stamp_lines(self) -> list[list[str]]:
        if not self.stamps.exists():
            return []
        return [line.split() for line in self.stamps.read_text().splitlines()]

    def starts(self) -> list[str]:
        return [fields[1] for fields in self.stamp_lines() if fields[0] == "start"]


class RunOrderTest(RunnerTestBase):
    def test_order_groups(self) -> None:
        """Five A and two B jobs given interleaved, fcheck on: every job and fcheck runs alone,
        each fcheck straight after its own job, every B after the last A (point (w))."""
        plan = [("a1", "A"), ("b1", "B"), ("a2", "A"), ("a3", "A"), ("b2", "B"), ("a4", "A"),
                ("a5", "A")]
        jobs = [self.job(d, [{"stamp": str(self.stamps), "name": d, "sleep": 0.05}], group=g)
                for d, g in plan]
        calls: list[tuple[str, int]] = []

        def fake_fcheck(job: Job, opts: qor_sched.RunOptions, verilator_j: int) -> str:
            with self.stamps.open("a") as handle:
                handle.write(f"start fcheck-{job.design} {time.time():.6f}\n")
            time.sleep(0.02)
            calls.append((job.design, verilator_j))
            with self.stamps.open("a") as handle:
                handle.write(f"end fcheck-{job.design} {time.time():.6f}\n")
            return "pass"

        with mock.patch.object(qor_sched, "run_fcheck", fake_fcheck):
            summary, _ = self.run_jobs(jobs, self.opts(fcheck=True))
        self.assertEqual(summary.exit_code, 0)
        order = ["a1", "a2", "a3", "a4", "a5", "b1", "b2"]
        self.assertEqual([r.job.design for r in summary.results], order)
        self.assertEqual(calls, [(d, 2 if d.startswith("a") else 1) for d in order])
        spans: dict[str, list[float]] = {}
        for _, name, when in self.stamp_lines():
            spans.setdefault(name, []).append(float(when))
        intervals = sorted((s, e, n) for n, (s, e) in spans.items())
        self.assertEqual([n for _, _, n in intervals],
                         [x for d in order for x in (d, f"fcheck-{d}")])
        for (_, end, name), (start, _, nxt) in zip(intervals, intervals[1:], strict=False):
            self.assertLessEqual(end, start, f"{name} overlaps {nxt}")
        last_a = max(e for _, e, n in intervals if n.removeprefix("fcheck-").startswith("a"))
        self.assertTrue(all(s >= last_a for s, _, n in intervals
                            if n.removeprefix("fcheck-").startswith("b")))

    def test_fcheck_after_cleanup_and_admitted(self) -> None:
        """The fcheck step sees the job already cleaned up (P's netlist gzipped) and is
        admitted like a job: memory short after the job makes it wait."""
        run = self.job_dir("sha") / "run"
        spec = {"write": [{"path": str(run / "sha.pre-vpr.blif"), "text": ".model sha\n"},
                          {"path": str(self.meminfo), "text": "MemAvailable: 1000 kB\n"}]}
        seen: list[list[str]] = []
        clock = FakeClock()
        clock.on_sleep = lambda n: self.set_mem(8.0)

        def fake_fcheck(job: Job, opts: qor_sched.RunOptions, verilator_j: int) -> str:
            seen.append(sorted(p.name for p in run.iterdir()))
            return "pass"

        with mock.patch.object(qor_sched, "run_fcheck", fake_fcheck):
            summary, lines = self.run_jobs([self.job("sha", [spec])],
                                           self.opts(clock=clock, sleep=clock.sleep,
                                                     fcheck=True))
        self.assertEqual(seen, [["sha.pre-vpr.blif.gz", "vtr_flow.out"]])
        self.assertIn("vtr-qor: waiting: 0.0 GiB available (< 7)", lines)
        self.assertEqual(clock.sleeps, [60])
        self.assertEqual(summary.exit_code, 0)

    def test_no_fcheck_unless_asked_or_ok(self) -> None:
        calls: list[str] = []
        jobs = [self.job("sha", [{}]), self.job("bad", [{"exit": 1}])]

        def fake_fcheck(job: Job, opts: qor_sched.RunOptions, verilator_j: int) -> str:
            calls.append(job.design)
            return "pass"

        with mock.patch.object(qor_sched, "run_fcheck", fake_fcheck):
            self.run_jobs(jobs, self.opts())
            self.assertEqual(calls, [])
            self.run_jobs(jobs, self.opts(fcheck=True))
        self.assertEqual(calls, ["sha"])

    def test_j_cap(self) -> None:
        jobs = [self.job("sha", [{"stamp": str(self.stamps), "name": "sha"}])]
        with self.assertRaises(HeavyError) as caught:
            self.run_jobs(jobs, self.opts(jobs=2))
        self.assertEqual((caught.exception.code, caught.exception.message),
                         (2, "-j 2 exceeds 1 (the heavy-run rule)"))
        self.assertEqual(qor_sched.MAX_JOBS, 1)
        self.assertEqual(self.starts(), [])
        self.assertFalse((self.out / "harness.log").exists())
        with self.assertRaises(HeavyError):
            self.run_jobs(jobs, self.opts(jobs=0))

    def test_constants(self) -> None:
        """The Global Constraints' scheduler caps."""
        self.assertEqual(
            (qor_sched.MIN_MEM_AVAILABLE_GIB, qor_sched.MIN_DISK_FREE_GIB,
             qor_sched.DISK_FLOOR_GIB, qor_sched.PAUSE_LIMIT_S, qor_sched.POLL_S,
             dict(qor_sched.JOB_TIMEOUT_S), qor_sched.KILL_GRACE_S, qor_sched.CAPPED_RETRIES,
             qor_sched.KEEP_KEYS, qor_sched.KEEP_FAILED, qor_sched.KEEP_FAILED_BYTES),
            (7, 20, 10, 7200, 60, {"A": 14400, "B": 21600}, 30, 0, 3, 5, 2 * GIB))
        self.assertEqual(qor_sched.KEEP_RUN_FILES, (
            "vtr_flow.out", "output.txt", "parse_results.txt", "qor_results.txt", "vpr.out",
            "vpr.crit_path.out", "parmys.out", "abc*.out", "*_blackboxing_latch.out",
            "restore_latch*.out", "report_clocks.abc.out", "parse_results.err",
            "qor_results.err"))

    def fcheck_exit(self, verdicts: list[str], jobs: list[Job], **kwargs: Any
                    ) -> qor_sched.RunSummary:
        queue = iter(verdicts)

        def fake_fcheck(job: Job, opts: qor_sched.RunOptions, verilator_j: int) -> str:
            return next(queue)

        with mock.patch.object(qor_sched, "run_fcheck", fake_fcheck):
            return self.run_jobs(jobs, self.opts(fcheck=True, **kwargs))[0]

    def test_failing_fcheck_shows_in_exit_code(self) -> None:
        """Review M4: a run whose jobs are ok but whose fcheck failed exits 1; capped or
        timed-out, 3; a pass, 0."""
        jobs = [self.job("sha", [{}])]
        self.assertEqual(self.fcheck_exit(["pass"], jobs).exit_code, 0)
        self.assertEqual(self.fcheck_exit(["fail"], jobs).exit_code, 1)
        self.assertEqual(self.fcheck_exit(["error"], jobs).exit_code, 1)
        self.assertEqual(self.fcheck_exit(["capped"], jobs).exit_code, 3)
        self.assertEqual(self.fcheck_exit(["timeout"], jobs).exit_code, 3)

    def test_cached_job_gets_its_missing_fcheck(self) -> None:
        """Review M4: ``--fcheck`` over a cached job with no fcheck verdict runs the check; a
        cached job that has one is left alone."""
        job = self.job("sha", [{}], cached=True)
        job.job_dir.mkdir(parents=True)
        (job.job_dir / "status").write_text("ok\n")
        record = {"status": "ok", "key": job.key, "verdicts": {"fcheck": "pass"}}
        (job.job_dir / "metrics.json").write_text(json.dumps({**record, "verdicts": {
            "fcheck": "skipped"}}))
        calls: list[str] = []

        def fake_fcheck(job: Job, opts: qor_sched.RunOptions, verilator_j: int) -> str:
            calls.append(job.design)
            return "fail"

        with mock.patch.object(qor_sched, "run_fcheck", fake_fcheck):
            summary, _ = self.run_jobs([job], self.opts(fcheck=True))
            self.assertEqual((calls, summary.exit_code, self.starts()), (["sha"], 1, []))
            (job.job_dir / "metrics.json").write_text(json.dumps(record))
            self.run_jobs([job], self.opts(fcheck=True))
        self.assertEqual(calls, ["sha"])  # the second pass found a verdict already

    def test_fcheck_not_started_after_deadline(self) -> None:
        """Review H3: the job's own fcheck does not start once the night's deadline passed."""
        calls: list[str] = []

        def fake_fcheck(job: Job, opts: qor_sched.RunOptions, verilator_j: int) -> str:
            calls.append(job.design)
            return "pass"

        job = self.job("sha", [{}])
        opts = self.opts(clock=lambda: 10.0, fcheck=True, deadline=10.0, deadline_text="06:00")
        run = qor_sched._Run([job], opts, finish_ok, qor_sched.HarnessLog(self.out))
        with mock.patch.object(qor_sched, "run_fcheck", fake_fcheck), \
                redirect_stderr(io.StringIO()):
            self.assertTrue(qor_sched._after_ok(run, job))
        self.assertEqual(calls, [])
        self.assertEqual(qor_sched._exit_code([], 0, run.fchecks), 3)

    def test_cached_job_not_run(self) -> None:
        """A cached job is not started; one whose metrics.json key is not the planned key is."""
        job = self.job("sha", [{"stamp": str(self.stamps), "name": "sha"}], cached=True)
        job.job_dir.mkdir(parents=True)
        (job.job_dir / "metrics.json").write_text(json.dumps({"status": "ok", "key": job.key}))
        (job.job_dir / "status").write_text("ok\n")
        summary, lines = self.run_jobs([job], self.opts())
        self.assertEqual(self.starts(), [])
        self.assertEqual([r.status for r in summary.results], ["ok"])
        self.assertIn("vtr-qor: [1/1] P sha cached", lines)
        (job.job_dir / "metrics.json").write_text(json.dumps({"status": "ok", "key": "0" * 64}))
        summary, _ = self.run_jobs([job], self.opts())
        self.assertEqual(self.starts(), ["sha"])
        self.assertTrue(qor_sched.job_complete(job))


FAKE_RANDOM_CHECK = """
import json, sys, time
from pathlib import Path
here = Path(__file__).resolve().parent
mode = json.loads((here / "mode.json").read_text())
args = sys.argv[1:]
net = Path(args[args.index("--post-synth-blif") + 1])
work = Path(args[args.index("--work-dir") + 1])
work.mkdir(parents=True, exist_ok=True)
(work / "marker.txt").write_text("work\\n")
with (here / "calls.jsonl").open("a") as log:
    log.write(json.dumps({"argv": args, "netlist": net.read_text(), "t0": time.time()}) + "\\n")
if "oom" in mode:
    events = Path(mode["oom"])
    events.write_text(events.read_text().replace("oom_kill 0", "oom_kill 1"))
if "sleep" in mode:
    time.sleep(mode["sleep"])
with (here / "calls.jsonl").open("a") as log:
    log.write(json.dumps({"end": args[args.index("--rtl") + 1], "t1": time.time()}) + "\\n")
sys.exit(mode.get("exit", 0))
"""


class FcheckTest(RunnerTestBase):
    """Plan Task 11: the Verilator random check of a finished job."""

    def setUp(self) -> None:
        super().setUp()
        self.vtr = self.tmp / "vtr"
        self.script = self.vtr / "mosaic" / "verilator_check" / "run_random_check.py"
        self.script.parent.mkdir(parents=True)
        self.script.write_text(FAKE_RANDOM_CHECK)
        self.mode({})

    def mode(self, mode: dict[str, Any]) -> None:
        (self.script.parent / "mode.json").write_text(json.dumps(mode))

    def calls(self) -> list[dict[str, Any]]:
        log = self.script.parent / "calls.jsonl"
        lines = log.read_text().splitlines() if log.exists() else []
        return [c for c in map(json.loads, lines) if "argv" in c]

    def ends(self) -> list[dict[str, Any]]:
        log = self.script.parent / "calls.jsonl"
        lines = log.read_text().splitlines() if log.exists() else []
        return [c for c in map(json.loads, lines) if "end" in c]

    def legctx(self, design: str, leg: str, group: str) -> qor_legs.LegContext:
        return qor_legs.LegContext(
            vtr_root=self.vtr, python=Path(sys.executable), arch_xml=self.vtr / "arch.xml",
            odin3=self.tmp / "odin3", repo=self.tmp / "repo", job_dir=self.job_dir(design, leg),
            k=6, group=group)

    def finished(self, design: str, *, leg: str = "P", group: str = "A", gz: bool = True,
                 netlist: str = ".model m\n.end\n") -> Job:
        """An ok job on disk (metrics.json, and the kept netlist as the runner leaves it)."""
        job = dataclasses.replace(self.job(design, [{}], leg=leg, group=group),
                                  ctx=self.legctx(design, leg, group))
        run = job.job_dir / "run"
        run.mkdir(parents=True, exist_ok=True)
        pre = run / f"{design}.pre-vpr.blif"
        if gz:
            with gzip.open(run / (pre.name + ".gz"), "wt") as handle:
                handle.write(netlist)
        else:
            pre.write_text(netlist)
        (job.job_dir / "metrics.json").write_text(json.dumps(
            {"schema": 1, "key": job.key, "status": "ok", "verdicts": {"fcheck": "skipped"}}))
        return job

    def verdict(self, job: Job) -> str:
        return str(json.loads((job.job_dir / "metrics.json").read_text())["verdicts"]["fcheck"])

    def test_fcheck_command(self) -> None:
        ctx = self.legctx("sha", "P", "A")
        netlist = ctx.job_dir / "fcheck" / "sha.pre-vpr.blif"
        cmd = qor_legs.fcheck_command(ctx, "sha", netlist, 2)
        self.assertEqual(list(cmd.argv), [
            sys.executable, f"{self.vtr}/mosaic/verilator_check/run_random_check.py",
            "--rtl", f"{self.vtr}/vtr_flow/benchmarks/verilog/sha.v",
            "--post-synth-blif", str(netlist), "--work-dir", f"{ctx.job_dir}/fcheck",
            "--vectors", "50000", "--seed", "1", "--verilator-j", "2"])
        self.assertEqual((cmd.cwd, cmd.stdout), (ctx.job_dir, ctx.job_dir / "fcheck.log"))
        self.assertEqual(dict(cmd.env)["PATH"].split(":")[0], f"{self.vtr}/.venv/bin")

    def test_fcheck_exit_mapping(self) -> None:
        for code, verdict in ((0, "pass"), (1, "fail"), (2, "error"), (3, "error"),
                              (137, "error")):
            self.mode({"exit": code})
            job = self.finished("sha")
            self.assertEqual(qor_sched.run_fcheck(job, self.opts(), 2), verdict, code)
            self.assertEqual(self.verdict(job), verdict)
        self.assertEqual(qor_legs.fcheck_verdict(137, True), "capped")
        self.assertEqual(qor_legs.fcheck_verdict(0, False), "pass")
        self.mode({"exit": 137, "oom": str(self.events)})
        job = self.finished("sha")
        self.assertEqual(qor_sched.run_fcheck(job, self.opts(), 2), "capped")
        self.assertEqual(self.verdict(job), "capped")

    def test_fcheck_timeout_is_not_error(self) -> None:
        """Review M2: a Verilator check stopped at the job limit is ``timeout``, not ``error``."""
        self.mode({"exit": 0, "sleep": 60})
        job = self.finished("sha")
        err = io.StringIO()
        with redirect_stderr(err):
            verdict = qor_sched.run_fcheck(
                job, self.opts(clock=time.monotonic, job_timeout_s={"A": 0.5, "B": 0.5}), 2)
        self.assertEqual((verdict, self.verdict(job)), ("timeout", "timeout"))

    def test_fcheck_stopped_at_the_night_deadline(self) -> None:
        """Review H3: the check is capped by the night's deadline, not only the job limit."""
        self.mode({"exit": 0, "sleep": 60})
        job = self.finished("sha")
        opts = self.opts(clock=time.monotonic, job_timeout_s={"A": 600, "B": 600},
                         deadline=time.monotonic() + 0.5, deadline_text="06:00")
        started = time.monotonic()
        with redirect_stderr(io.StringIO()):
            verdict = qor_sched.run_fcheck(job, opts, 2)
        self.assertEqual(verdict, "timeout")
        self.assertLess(time.monotonic() - started, 30)

    def test_fcheck_p_from_gz(self) -> None:
        job = self.finished("sha", netlist=".model sha\n.names a b\n11 1\n.end\n")
        self.assertEqual(qor_sched.run_fcheck(job, self.opts(), 2), "pass")
        (call,) = self.calls()
        argv = call["argv"]
        self.assertEqual(argv[argv.index("--post-synth-blif") + 1],
                         str(job.job_dir / "fcheck" / "sha.pre-vpr.blif"))
        self.assertEqual(call["netlist"], ".model sha\n.names a b\n11 1\n.end\n")

    def test_fcheck_other_leg_needs_kept_netlist(self) -> None:
        job = self.finished("sha", leg="O", gz=False)
        self.assertEqual(qor_sched.run_fcheck(job, self.opts(), 2), "pass")
        (call,) = self.calls()
        self.assertEqual(call["argv"][call["argv"].index("--post-synth-blif") + 1],
                         str(job.job_dir / "run" / "sha.pre-vpr.blif"))
        gone = self.finished("stereovision3", leg="O", gz=True)  # only a .gz: not a kept pre-vpr
        err = io.StringIO()
        with redirect_stderr(err):
            self.assertEqual(qor_sched.run_fcheck(gone, self.opts(), 2), "error")
        self.assertIn("vtr-qor: fcheck: O stereovision3: no kept netlist (run with "
                      "--keep-netlists)", err.getvalue())
        self.assertEqual(self.verdict(gone), "error")
        self.assertEqual(len(self.calls()), 1)  # none for stereovision3

    def test_fcheck_dir_kept_on_fail(self) -> None:
        passed = self.finished("sha")
        qor_sched.run_fcheck(passed, self.opts(), 2)
        self.assertFalse((passed.job_dir / "fcheck").exists())
        self.mode({"exit": 1})
        failed = self.finished("diffeq1")
        qor_sched.run_fcheck(failed, self.opts(), 2)
        self.assertTrue((failed.job_dir / "fcheck" / "marker.txt").is_file())

    def test_fcheck_refused_outside_heavy_run(self) -> None:
        job = self.finished("sha")
        with self.assertRaises(HeavyError):
            qor_sched.run_fcheck(job, self.opts(scope=None), 2)

    def test_fcheck_after_own_job(self) -> None:
        """Each fcheck starts after its own VPR job ends and ends before the next job starts;
        group B gets --verilator-j 1, group A 2."""
        specs = {d: {"stamp": str(self.stamps), "name": d,
                     "write": [{"path": str(self.job_dir(d, "O") / "run" / f"{d}.pre-vpr.blif"),
                                "text": ".model m\n.end\n"}]}
                 for d in ("sha", "mkPktMerge", "bgm")}
        self.mode({"sleep": 0.05})
        jobs = [dataclasses.replace(self.job(d, [specs[d]], leg="O", group=g),
                                    ctx=self.legctx(d, "O", g))
                for d, g in (("sha", "A"), ("mkPktMerge", "A"), ("bgm", "B"))]
        summary, _ = self.run_jobs(jobs, self.opts(fcheck=True, keep_netlists=frozenset({"O"})))
        self.assertEqual([r.status for r in summary.results], ["ok"] * 3)
        stamps = {(f[0], f[1]): float(f[2]) for f in self.stamp_lines()}
        starts, ends = self.calls(), self.ends()
        self.assertEqual([c["argv"][c["argv"].index("--verilator-j") + 1] for c in starts],
                         ["2", "2", "1"])
        order = ["sha", "mkPktMerge", "bgm"]
        for i, design in enumerate(order):
            self.assertGreater(starts[i]["t0"], stamps[("end", design)])
            self.assertLess(ends[i]["t1"], stamps.get(("start", order[i + 1]), 1e18)
                            if i + 1 < len(order) else 1e18)
        self.assertEqual([self.verdict(j) for j in jobs], ["pass"] * 3)


FAKE_EQUIV_CHECK = """
import json, sys, time
from pathlib import Path
here = Path(__file__).resolve().parent
mode = json.loads((here / "mode.json").read_text())
with (here / "calls.jsonl").open("a") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
time.sleep(mode.get("sleep", 0))
sys.stderr.write(mode.get("stderr", ""))
sys.exit(mode.get("exit", 0))
"""


class EquivTest(RunnerTestBase):
    """Plan Task 12: ``equiv-check`` between the O' and O netlists."""

    def setUp(self) -> None:
        super().setUp()
        self.repo = self.tmp / "repo"
        self.tool = self.repo / "tools" / "equiv-check" / "equiv-check"
        self.tool.parent.mkdir(parents=True)
        self.tool.write_text(f"#!{sys.executable}\n{FAKE_EQUIV_CHECK}")
        self.tool.chmod(0o755)
        self.mode({})

    def mode(self, mode: dict[str, Any]) -> None:
        (self.tool.parent / "mode.json").write_text(json.dumps(mode))

    def calls(self) -> list[list[str]]:
        log = self.tool.parent / "calls.jsonl"
        return [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []

    def pair(self, design: str, *, ram: int = 0, keep: bool = True) -> tuple[Job, Job]:
        jobs = []
        for leg in ("O", "Oprime"):
            ctx = qor_legs.LegContext(
                vtr_root=self.tmp / "vtr", python=Path(sys.executable), arch_xml=self.tmp / "a",
                odin3=self.tmp / "odin3", repo=self.repo, job_dir=self.job_dir(design, leg),
                k=6, group="A")
            job = dataclasses.replace(self.job(design, [{}], leg=leg), ctx=ctx)
            job.job_dir.mkdir(parents=True, exist_ok=True)
            if keep:
                (job.job_dir / f"{design}.blif").write_text(f".model {leg}\n.end\n")
            (job.job_dir / "metrics.json").write_text(json.dumps({
                "schema": 1, "key": job.key, "status": "ok", "verdicts": {"equiv": "skipped"},
                "netlist": {"ram_slices": ram}}))
            jobs.append(job)
        return jobs[0], jobs[1]

    def verdict(self, job: Job) -> str:
        return str(json.loads((job.job_dir / "metrics.json").read_text())["verdicts"]["equiv"])

    def test_equiv_command(self) -> None:
        repo, before, after = Path("/r"), Path("/j/Oprime/sha/sha.blif"), Path("/j/O/sha/sha.blif")
        self.assertEqual(qor_sched.equiv_command(repo, before, after), [
            "/r/tools/equiv-check/equiv-check", "--timeout", "600", str(before), str(after)])
        self.assertEqual((qor_sched.EQUIV_TIMEOUT_S, qor_sched.EQUIV_WALL_S), (600, 660))

    def test_equiv_classification(self) -> None:
        cases = [
            (0, "", "equivalent"), (1, "equiv-check: NOT equivalent", "differs"),
            (2, "equiv-check: unproven: x: unsupported: more than one clock domain (type, "
                "clock): [('re', 'a'), ('re', 'b')]", "multiclock"),
            (4, "unproven: x: unsupported: more than one clock domain", "multiclock"),
            (2, "equiv-check: running ABC failed: timed out", "timeout"),
            (4, "unproven: ABC dsec timed out after 600 seconds", "timeout"),
            (4, "unproven: ABC dsec ended UNDECIDED", "timeout"),
            (None, "", "timeout"),
            (2, "equiv-check: cannot read a.blif", "error"),
            (2, "running ABC failed: no such file", "error"),
            (5, "ABC killed by signal 9", "capped"), (3, "", "error"),
        ]
        for code, err, expected in cases:
            self.assertEqual(qor_sched.equiv_verdict(code, err), expected, (code, err))

    def test_equiv_runs_the_tool(self) -> None:
        """The fake tool's exit and stderr give each verdict, written to the O job; the argv is
        O' then O."""
        for code, err, expected in ((0, "", "equivalent"), (1, "", "differs"),
                                    (2, "unsupported: more than one clock domain", "multiclock"),
                                    (4, "timed out", "timeout"), (2, "boom", "error")):
            self.mode({"exit": code, "stderr": err})
            o_job, prime = self.pair("sha")
            self.assertEqual(qor_sched.run_equiv("sha", o_job, prime, self.opts()), expected)
            self.assertEqual(self.verdict(o_job), expected)
            self.assertEqual((o_job.job_dir / "equiv.err").read_text(), err)
        self.assertEqual(self.calls()[-1], [
            "--timeout", "600", str(self.job_dir("sha", "Oprime") / "sha.blif"),
            str(self.job_dir("sha", "O") / "sha.blif")])

    def test_equiv_stopped_at_the_night_deadline(self) -> None:
        """Review H3: ``equiv-check`` is killed at the night's deadline, well before its own
        wall cap."""
        o_job, prime = self.pair("sha")
        self.mode({"exit": 0, "sleep": 60})
        opts = self.opts(clock=time.monotonic, deadline=time.monotonic() + 0.5,
                         deadline_text="06:00")
        started = time.monotonic()
        with redirect_stderr(io.StringIO()):
            verdict = qor_sched.run_equiv("sha", o_job, prime, opts)
        self.assertEqual(verdict, "timeout")
        self.assertLess(time.monotonic() - started, 30)

    def test_checks_not_started_after_deadline(self) -> None:
        """Review H3: past the deadline ``run_checks`` starts nothing and says so (exit 3)."""
        o_job, _ = self.pair("sha")
        ran: list[str] = []

        def step(job: Job) -> str:
            ran.append(job.design)
            return "equivalent"

        err = io.StringIO()
        opts = self.opts(clock=lambda: 10.0, deadline=10.0, deadline_text="06:00")
        with redirect_stderr(err):
            verdicts, code = qor_sched.run_checks([o_job], opts, step)
        self.assertEqual((verdicts, code, ran), ([], 3, []))
        self.assertIn("1 checks not started before the deadline 06:00", err.getvalue())

    def test_capped_deadline(self) -> None:
        opts = self.opts(clock=lambda: 100.0)
        self.assertEqual(qor_sched.capped_deadline(opts, 60), 160.0)
        opts.deadline = 130.0
        self.assertEqual(qor_sched.capped_deadline(opts, 60), 130.0)
        self.assertFalse(qor_sched.past_deadline(opts))
        opts.deadline = 100.0
        self.assertTrue(qor_sched.past_deadline(opts))

    def test_equiv_ram_skipped(self) -> None:
        o_job, prime = self.pair("diffeq1", ram=4)
        self.assertEqual(qor_sched.run_equiv("diffeq1", o_job, prime, self.opts()), "ram")
        self.assertEqual(self.verdict(o_job), "ram")
        self.assertEqual(self.calls(), [])
        self.assertFalse((o_job.job_dir / "equiv.log").exists())

    def test_equiv_wall_cap_and_missing_netlist(self) -> None:
        o_job, prime = self.pair("sha")
        self.mode({"exit": 0, "sleep": 60})  # outlives the (patched) wall cap and is killed
        err = io.StringIO()
        with mock.patch.object(qor_sched, "EQUIV_WALL_S", 0.5), redirect_stderr(err):
            verdict = qor_sched.run_equiv("sha", o_job, prime, self.opts(clock=time.monotonic))
        self.assertEqual((verdict, self.verdict(o_job)), ("timeout", "timeout"))
        gone_o, gone_prime = self.pair("stereovision3", keep=False)
        err = io.StringIO()
        with redirect_stderr(err):
            self.assertEqual(qor_sched.run_equiv("stereovision3", gone_o, gone_prime,
                                                 self.opts()), "error")
        self.assertIn("vtr-qor: equiv: O' stereovision3: no kept netlist", err.getvalue()
                      .replace("Oprime", "O'"))
        self.assertEqual(self.verdict(gone_o), "error")

    def test_equiv_refused_outside_heavy_run(self) -> None:
        o_job, prime = self.pair("sha")
        with self.assertRaises(HeavyError):
            qor_sched.run_equiv("sha", o_job, prime, self.opts(scope=None))


class HeavyGuardTest(RunnerTestBase):
    """Nothing starts unless the real nesting evidence and the cap hold (standing rules)."""

    def assert_refused(self, call: Callable[[], object]) -> None:
        with self.assertRaises(HeavyError) as caught:
            call()
        self.assertEqual(caught.exception.code, 2)
        self.assertEqual(self.starts(), [])

    def test_refuses_outside_heavy_run(self) -> None:
        job = self.job("sha", [{"stamp": str(self.stamps), "name": "sha"}])
        no_scope = self.opts(scope=None)
        self.assert_refused(lambda: self.run_jobs([job], no_scope))
        self.assert_refused(lambda: qor_sched.run_one(job, no_scope, finish_ok))
        self.assert_refused(lambda: qor_sched.run_command(job.commands[0], no_scope,
                                                          deadline=time.time() + 60))
        self.assertFalse((self.out / "harness.log").exists())
        # ODIN3_HEAVY_SLICE with a scope object, but descriptor 9 is not the lock: not inside.
        with mock.patch.object(qor_heavy, "FD9", self.tmp / "no-fd9"):
            self.assert_refused(lambda: self.run_jobs([job], self.opts()))
            self.assert_refused(lambda: qor_sched.run_one(job, self.opts(), finish_ok))
        # A cgroup outside the slice: not inside either.
        with mock.patch.object(qor_heavy, "SELF_CGROUP", self.tmp / "other-cgroup"):
            (self.tmp / "other-cgroup").write_text("0::/user.slice/other.scope\n")
            self.assert_refused(lambda: self.run_jobs([job], self.opts()))
        # Inside, but the cap is gone.
        (self.slice / "memory.max").write_text("max\n")
        self.assert_refused(lambda: self.run_jobs([job], self.opts()))

    def test_job_dirs_under_out(self) -> None:
        job = self.job("sha", [{"stamp": str(self.stamps), "name": "sha"}])
        stray = dataclasses.replace(job, job_dir=self.tmp / "elsewhere" / "a" / "b" / "c" / "d")
        self.assert_refused(lambda: self.run_jobs([stray], self.opts()))
        shallow = dataclasses.replace(job, job_dir=self.out / "a" / "b" / "c" / "d")
        self.assert_refused(lambda: self.run_jobs([shallow], self.opts()))
        # An unknown group is refused, never silently dropped from the run.
        self.assert_refused(lambda: self.run_jobs([job, dataclasses.replace(job, group="C")],
                                                  self.opts()))
        self.assert_refused(lambda: self.run_jobs([job], self.opts(job_timeout_s={"B": 1})))


class CommandTest(RunnerTestBase):
    def test_env_and_streams(self) -> None:
        """Command.stderr gets stderr alone; without it stderr joins stdout. The job sees its
        overrides and PYTHONDONTWRITEBYTECODE=1; output files are written afresh."""
        work = self.tmp / "w"
        work.mkdir()
        dump = self.tmp / "env.json"
        out, err = work / "parse_results.txt", work / "parse_results.err"
        out.write_text("old contents\n")
        spec = {"stdout": "OUT\n", "stderr": "ERR\n", "env_dump": str(dump)}
        cmd = self.command(spec, out, stderr=err, env=(("PYTHONPATH", "/v/libs"),))
        result = qor_sched.run_command(cmd, self.opts(env={"PATH": os.defpath, "X": "1"}),
                                       deadline=FakeClock().now + 60)
        self.assertEqual((result.kind, result.returncode), ("exit", 0))
        self.assertEqual((out.read_text(), err.read_text()), ("OUT\n", "ERR\n"))
        env = json.loads(dump.read_text())
        self.assertEqual((env["PYTHONDONTWRITEBYTECODE"], env["PYTHONPATH"], env["X"]),
                         ("1", "/v/libs", "1"))
        merged = work / "merged.log"
        cmd = self.command(spec, merged)
        qor_sched.run_command(cmd, self.opts(), deadline=FakeClock().now + 60)
        self.assertEqual(sorted(merged.read_text().splitlines()), ["ERR", "OUT"])

    def test_missing_program(self) -> None:
        job = self.job("sha", [])
        bad = qor_legs.Command((str(self.tmp / "nope"),), self.tmp, self.tmp / "x.log", ())
        job = dataclasses.replace(job, commands=(bad,))
        result = qor_sched.run_one(job, self.opts(), finish_ok)
        self.assertEqual(result.status, "failed")
        self.assertEqual(result.error,
                         f"P sha: cannot run {self.tmp / 'nope'}: No such file or directory")

    def test_group_stragglers_killed(self) -> None:
        """A job that exits 0 but leaves a grandchild running: the grandchild is killed before
        the next job (one job at a time)."""
        pids = self.tmp / "pids"
        job = self.job("sha", [{"spawn_sleep": 600, "pids": str(pids)}])
        result = qor_sched.run_one(job, self.opts(), finish_ok)
        self.assertEqual(result.status, "ok")
        self.assertTrue(_wait_dead([int(p) for p in pids.read_text().split()]))

    def test_interrupt_kills_group(self) -> None:
        """An exception while a job runs (Ctrl-C, SIGTERM as SystemExit) kills its group."""
        pids = self.tmp / "pids"
        job = self.job("sha", [{"spawn_sleep": 600, "pids": str(pids), "sleep": 600}])
        calls = 0

        def clock() -> float:
            nonlocal calls
            calls += 1
            if pids.exists() and calls > 3:
                raise KeyboardInterrupt
            return 1_000_000.0

        with self.assertRaises(KeyboardInterrupt):
            qor_sched.run_one(job, self.opts(clock=clock), finish_ok)
        self.assertTrue(_wait_dead([int(p) for p in pids.read_text().split()]))

    def test_sigterm_handler_restored(self) -> None:
        before = signal.getsignal(signal.SIGTERM)
        self.run_jobs([self.job("sha", [{}])], self.opts())
        self.assertIs(signal.getsignal(signal.SIGTERM), before)

    def test_sigterm_while_running(self) -> None:
        """SIGTERM to the harness while a job runs: the job's group is killed, then the run
        exits 143."""
        pids = self.tmp / "pids"
        job = self.job("sha", [{"spawn_sleep": 600, "pids": str(pids), "sleep": 600}])
        sent = False

        def clock() -> float:
            nonlocal sent
            if pids.exists() and not sent:
                sent = True
                os.kill(os.getpid(), signal.SIGTERM)
            return 1_000_000.0

        with self.assertRaises(SystemExit) as caught:
            self.run_jobs([job], self.opts(clock=clock))
        self.assertEqual(caught.exception.code, 143)
        self.assertTrue(_wait_dead([int(p) for p in pids.read_text().split()]))

    def test_term_before_kill(self) -> None:
        """On timeout the group gets SIGTERM first; a job that exits on it is not waited for
        through the whole grace period."""
        flag, pids = self.tmp / "term", self.tmp / "pids"
        spec = {"on_term": str(flag), "spawn_sleep": 600, "pids": str(pids), "sleep": 600}
        began = frozen = time.monotonic()
        opts = self.opts(clock=lambda: time.monotonic() if pids.exists() else frozen,
                         job_timeout_s={"A": 1, "B": 1})
        with mock.patch.object(qor_sched, "KILL_GRACE_S", 60.0):
            result = qor_sched.run_one(self.job("sha", [spec]), opts, finish_ok)
        self.assertEqual(result.status, "timeout")
        self.assertEqual(flag.read_text(), "term\n")
        self.assertLess(time.monotonic() - began, 30)
        self.assertTrue(_wait_dead([int(p) for p in pids.read_text().split()]))

    def test_lock_descriptor_passed(self) -> None:
        """Descriptor 9 reaches the job when it really is the heavy lock (so an orphaned job
        keeps the lock), and only then."""
        try:
            os.fstat(9)
            self.skipTest("descriptor 9 is in use in this process")
        except OSError:
            pass
        dump = self.tmp / "fds"
        cmd = self.command({"fd_dump": str(dump)}, self.tmp / "x.log")
        other = self.tmp / "other"
        other.touch()
        fd = os.open(self.lock, os.O_RDONLY)
        try:
            os.dup2(fd, 9)
            qor_sched.run_command(cmd, self.opts(), deadline=FakeClock().now + 60)
            self.assertIn(f"9 {self.lock}", dump.read_text().splitlines())
            os.close(fd)
            fd = os.open(other, os.O_RDONLY)
            os.dup2(fd, 9)
            qor_sched.run_command(cmd, self.opts(), deadline=FakeClock().now + 60)
            self.assertFalse(any(line.startswith("9 ") for line in
                                 dump.read_text().splitlines()))
        finally:
            os.close(9)
            os.close(fd)


class OutcomeTest(RunnerTestBase):
    def test_capped_no_verdict(self) -> None:
        """A failure while oom_kill rose is capped, with the leaf scope's peak (not the
        slice's), then no verdict; no retry; exit 3. Without the rise it is failed, exit 1."""
        (self.leaf / "memory.peak").write_text(f"{6000 * MIB}\n")
        (self.slice / "memory.peak").write_text(f"{6100 * MIB}\n")
        vpr_out = str(self.job_dir("sha") / "run" / "vpr.out")
        parmys = str(self.job_dir("sha") / "run" / "parmys.out")
        spec = {"stamp": str(self.stamps), "name": "sha", "exit": 1,
                "write": [{"path": parmys, "text": "done\n", "age_s": 60},
                          {"path": vpr_out, "text": "routing\n"}],
                "oom_kill": {"events": str(self.events), "add": 1}}
        summary, lines = self.run_jobs([self.job("sha", [spec])], self.opts())
        self.assertEqual([r.status for r in summary.results], ["capped"])
        capped = ("vtr-qor: P sha: capped: stage vpr.out exceeded the 6 GiB scope "
                  "(peak ≥ 6000 MiB at the kill)")
        self.assertEqual(lines[:3], [capped, "vtr-qor: P sha: no verdict",
                                     "vtr-qor: [1/1] P sha capped 0.0 s (A, j1)"])
        self.assertEqual(self.starts(), ["sha"])
        self.assertEqual(summary.exit_code, 3)
        self.assertEqual((self.job_dir("sha") / "status").read_text(), "capped\n")
        self.assertTrue(Path(vpr_out).exists())
        self.assertFalse((self.job_dir("sha") / "metrics.json").exists())
        # The counter is cumulative: an unchanged (non-zero) count is no rise.
        spec.pop("oom_kill")
        summary, lines = self.run_jobs([self.job("sha", [spec])], self.opts())
        self.assertEqual([r.status for r in summary.results], ["failed"])
        self.assertEqual(summary.exit_code, 1)
        flow = self.job_dir("sha") / "run" / "vtr_flow.out"
        python = Path(sys.executable).name
        self.assertEqual(lines[0], f"vtr-qor: P sha: {python} exited 1; see {flow}")

    def test_capped_unknown_peak_and_xl(self) -> None:
        spec = {"exit": 137, "oom_kill": {"events": str(self.events), "add": 2}}
        result = qor_sched.run_one(self.job("sha", [spec]), self.opts(), finish_ok)
        self.assertEqual(result.error, "P sha: capped: stage ? exceeded the 6 GiB scope "
                                       "(peak ≥ ? MiB at the kill)")

    def test_fresh_attempt_mtime(self) -> None:
        job_dir = self.job_dir("sha")
        (job_dir / "run").mkdir(parents=True)
        for name in ("run/vpr.out", "run/parse_results.txt", "metrics.json", "status",
                     "job.json", "sha.blif", "frontend.log"):
            (job_dir / name).write_text("previous attempt\n")
        job = self.job("sha", [])
        before = time.time()
        start = qor_sched.fresh_attempt(job)
        self.assertLessEqual(start, time.time())
        self.assertGreaterEqual(start, before - 0.1)
        self.assertEqual(list(job_dir.iterdir()), [job_dir / "run"])
        self.assertEqual(list((job_dir / "run").iterdir()), [])
        parse = str(job_dir / "run" / "parse_results.txt")

        def finish(job: Job, attempt_start: float) -> dict[str, object]:
            fields = qor_parse.read_parse_results(Path(parse), not_before=attempt_start,
                                                  label=f"{job.leg} {job.design}")
            return {"routed": {"vpr_status": fields["vpr_status"]}}

        stale = self.job("sha", [{"write": [{"path": parse, "text": PARSE_TEXT, "age_s": 60}]}])
        summary, lines = self.run_jobs([stale], self.opts(), finish)
        self.assertEqual([r.status for r in summary.results], ["failed"])
        message = "P sha: stale parse_results.txt from an earlier attempt; job failed"
        self.assertEqual(summary.results[0].error, message)
        self.assertEqual(lines[0], f"vtr-qor: {message}")
        self.assertFalse((job_dir / "metrics.json").exists())
        self.assertEqual((job_dir / "status").read_text(), "failed\n")
        fresh = self.job("sha", [{"write": [{"path": parse, "text": PARSE_TEXT}]}])
        summary, _ = self.run_jobs([fresh], self.opts(), finish)
        self.assertEqual([r.status for r in summary.results], ["ok"])

    def test_timeout_kills_group(self) -> None:
        """A job (and its grandchild) ignoring SIGTERM past a 1 s timeout: SIGTERM to the group,
        SIGKILL after KILL_GRACE_S (0.5 s here); neither PID survives."""
        pids = self.tmp / "pids"
        spec = {"ignore_term": True, "spawn_sleep": 600, "pids": str(pids), "sleep": 600}
        # One monotonic clock for the job's deadline and for ``took``, held at ``began`` until
        # the fake ignores SIGTERM (it may start slowly under load): the 1 s timeout then counts
        # from no earlier than ``began`` and the 0.5 s grace follows it, so took >= 1.5 holds
        # however the run is scheduled. (The wall clock read before ``began`` let a preemption
        # or a WSL clock step eat that margin.)
        began = frozen = time.monotonic()

        def clock() -> float:
            return time.monotonic() if pids.exists() else frozen

        opts = self.opts(clock=clock, sleep=time.sleep, job_timeout_s={"A": 1, "B": 1})
        with mock.patch.object(qor_sched, "KILL_GRACE_S", 0.5):
            summary, lines = self.run_jobs([self.job("sha", [spec])], opts)
        took = time.monotonic() - began
        self.assertEqual([r.status for r in summary.results], ["timeout"])
        self.assertIn("vtr-qor: P sha: timeout after 1 s", lines)
        self.assertIn("vtr-qor: P sha: timeout after 1 s",
                      (self.out / "harness.log").read_text().splitlines())
        self.assertGreaterEqual(took, 1.5)
        self.assertLess(took, 60)  # far below the fake's 600 s: the kill, not the job, ended it
        self.assertTrue(_wait_dead([int(p) for p in pids.read_text().split()]))
        self.assertEqual(summary.exit_code, 3)
        self.assertEqual((self.job_dir("sha") / "status").read_text(), "timeout\n")

    def test_failure_texts(self) -> None:
        front = {"exit": 3, "stamp": str(self.stamps), "name": "front"}
        vtr = {"stamp": str(self.stamps), "name": "vtr"}
        summary, lines = self.run_jobs([self.job("sha", [front, vtr], leg="O")], self.opts())
        log = self.job_dir("sha", "O") / "frontend.log"
        self.assertEqual(summary.results[0].status, "failed")
        self.assertEqual(lines[0], f"vtr-qor: O sha: front end exited 3; see {log}")
        self.assertEqual(self.starts(), ["front"])
        self.assertEqual(summary.exit_code, 1)

        def finish(job: Job, start: float) -> dict[str, object]:
            return {"routed": {"vpr_status": "failed"}, "key": "stale"}

        job = self.job("sha", [{}])
        summary, lines = self.run_jobs([job], self.opts(), finish)
        flow = self.job_dir("sha") / "run" / "vtr_flow.out"
        self.assertEqual(lines[0], f"vtr-qor: P sha: vpr_status=failed; see {flow}")
        metrics = json.loads((job.job_dir / "metrics.json").read_text())
        self.assertEqual((metrics["status"], metrics["key"]), ("failed", job.key))
        self.assertFalse(qor_sched.job_complete(job))
        summary, lines = self.run_jobs([job], self.opts(), lambda j, s: {"routed": {}})
        self.assertEqual(lines[0], f"vtr-qor: P sha: vpr_status=missing; see {flow}")

    def test_finish_errors(self) -> None:
        """A finish that raises fails the job; a metrics.json it left behind does not survive."""
        job = self.job("sha", [{}])

        def finish(job: Job, start: float) -> dict[str, object]:
            (job.job_dir / "metrics.json").write_text(json.dumps({"status": "ok",
                                                                  "key": job.key}))
            raise ValueError("bad number")

        result = qor_sched.run_one(job, self.opts(), finish)
        self.assertEqual((result.status, result.error),
                         ("failed", "P sha: cannot finish the job: bad number"))
        self.assertFalse((job.job_dir / "metrics.json").exists())
        self.assertFalse(qor_sched.job_complete(job))
        result = qor_sched.run_one(job, self.opts(), lambda j, s: {"x": object()})
        self.assertEqual(result.status, "failed")
        self.assertTrue(result.error.startswith("P sha: cannot finish the job: "))

    def test_ok_metrics_and_status(self) -> None:
        job = self.job("sha", [{}])
        clock = FakeClock(tick=0.75)
        result = qor_sched.run_one(job, self.opts(clock=clock), finish_ok)
        self.assertEqual(result.status, "ok")
        metrics = json.loads((job.job_dir / "metrics.json").read_text())
        self.assertEqual((metrics["status"], metrics["key"], metrics["error"], metrics["leg"]),
                         ("ok", job.key, "", "P"))
        self.assertEqual(metrics["elapsed_s"], round(result.elapsed_s, 1))
        self.assertTrue((job.job_dir / "metrics.json").read_text().startswith('{\n "elapsed_s"'))
        self.assertEqual((job.job_dir / "status").read_text(), "ok\n")
        self.assertTrue(qor_sched.job_complete(job))


class CgroupKillTest(RunnerTestBase):
    """Review C1: VTR starts every stage in a session of its own, so a process-group kill misses
    it; the job cgroup's drain must not. The fake records its grandchild in the fake
    cgroup.procs as the kernel would."""

    def setUp(self) -> None:
        super().setUp()
        self.cg = self.leaf / qor_sched.JOB_CGROUP
        self.pids = self.tmp / "pids"

    def stage(self, **extra: Any) -> Spec:
        """A fake run_vtr_flow: a stage in its own session, then it waits."""
        spec: Spec = {"spawn_sleep": 600, "spawn_session": True, "pids": str(self.pids),
                      "cgroup_procs": str(self.cg / "cgroup.procs"), "sleep": 600}
        spec.update(extra)
        return spec

    def pid_list(self) -> list[int]:
        return [int(p) for p in self.pids.read_text().split()]

    def held_clock(self) -> Callable[[], float]:
        """Real time, held until the stage runs; records the two sessions once."""
        frozen = time.monotonic()
        self.sessions: list[int] = []

        def clock() -> float:
            if not self.pids.exists():
                return frozen
            if not self.sessions:
                self.sessions = [os.getsid(pid) for pid in self.pid_list()]
            return time.monotonic()

        return clock

    def test_job_joins_cgroup(self) -> None:
        spec = {"spawn_sleep": 0, "pids": str(self.pids)}
        result = qor_sched.run_one(self.job("sha", [spec]), self.opts(), finish_ok)
        self.assertEqual(result.status, "ok")
        leader = self.pid_list()[0]
        self.assertIn(str(leader), (self.cg / "cgroup.procs").read_text().split())

    def test_setsid_stage_killed_on_timeout(self) -> None:
        opts = self.opts(clock=self.held_clock(), sleep=time.sleep,
                         job_timeout_s={"A": 1, "B": 1})
        with mock.patch.object(qor_sched, "KILL_GRACE_S", 0.5):
            summary, lines = self.run_jobs([self.job("sha", [self.stage()]),
                                            self.job("next", [{"stamp": str(self.stamps),
                                                               "name": "next"}])], opts)
        self.assertEqual(len(set(self.sessions)), 2)  # the stage really had its own session
        self.assertEqual([r.status for r in summary.results], ["timeout", "ok"])
        self.assertTrue(_wait_dead(self.pid_list(), 0.1))  # dead before the next job ran
        self.assertIn("vtr-qor: P sha: timeout after 1 s", lines)

    def test_setsid_stage_killed_on_floor(self) -> None:
        spec = self.stage(write=[{"path": str(self.df_file), "text": "9\n"}])
        summary, _ = self.run_jobs([self.job("sha", [spec])], self.opts(clock=ScaledClock(600)))
        self.assertEqual(([r.status for r in summary.results], summary.exit_code),
                         (["aborted"], 5))
        self.assertTrue(_wait_dead(self.pid_list(), 0.1))

    def test_setsid_stage_killed_on_sigterm(self) -> None:
        sent = False
        inner = self.held_clock()

        def clock() -> float:
            nonlocal sent
            now = inner()
            if self.pids.exists() and not sent:
                sent = True
                os.kill(os.getpid(), signal.SIGTERM)
            return now

        with self.assertRaises(SystemExit):
            self.run_jobs([self.job("sha", [self.stage()])], self.opts(clock=clock))
        self.assertTrue(_wait_dead(self.pid_list(), 0.1))

    def test_setsid_straggler_after_exit(self) -> None:
        """A job that exits 0 leaving a stage in its own session: the stage is killed before
        run_one returns."""
        result = qor_sched.run_one(self.job("sha", [self.stage(sleep=0)]), self.opts(),
                                   finish_ok)
        self.assertEqual(result.status, "ok")
        self.assertTrue(_wait_dead(self.pid_list(), 0.1))

    def test_stage_gets_sigterm_and_its_grace(self) -> None:
        """On a timeout the stage in its own session gets SIGTERM, and the grace lasts until the
        cgroup is empty (not just until run_vtr_flow, which dies at once, has gone)."""
        flag = self.tmp / "stage-term"
        opts = self.opts(clock=self.held_clock(), sleep=time.sleep,
                         job_timeout_s={"A": 1, "B": 1})
        with mock.patch.object(qor_sched, "KILL_GRACE_S", 20.0):
            result = qor_sched.run_one(self.job("sha", [self.stage(stage_on_term=str(flag))]),
                                       opts, finish_ok)
        self.assertEqual(result.status, "timeout")
        self.assertEqual(flag.read_text(), "term\n")
        self.assertTrue(_wait_dead(self.pid_list(), 0.1))

    def test_cgroup_kill_written(self) -> None:
        self.cg.mkdir()
        (self.cg / "cgroup.kill").write_text("")
        opts = self.opts(clock=self.held_clock(), sleep=time.sleep,
                         job_timeout_s={"A": 1, "B": 1})
        with mock.patch.object(qor_sched, "KILL_GRACE_S", 0.2):
            result = qor_sched.run_one(self.job("sha", [self.stage(ignore_term=True)]), opts,
                                       finish_ok)
        self.assertEqual(result.status, "timeout")
        self.assertEqual((self.cg / "cgroup.kill").read_text(), "1")
        self.assertTrue(_wait_dead(self.pid_list(), 0.1))

    def test_busy_scope_refused(self) -> None:
        """Another process in the harness's scope or in the job cgroup: no job starts."""
        sleeper = subprocess.Popen([sys.executable, "-I", "-c", "import time; time.sleep(60)"])
        self.addCleanup(sleeper.wait)
        self.addCleanup(sleeper.kill)
        job = self.job("sha", [{"stamp": str(self.stamps), "name": "sha"}])
        for where in (self.leaf, self.cg):
            where.mkdir(exist_ok=True)
            (where / "cgroup.procs").write_text(f"{os.getpid()}\n{sleeper.pid}\n")
            summary, lines = self.run_jobs([job], self.opts())
            self.assertEqual(lines[0], f"vtr-qor: one job at a time: {where} still holds "
                                       f"process {sleeper.pid}; not starting another")
            self.assertEqual((summary.exit_code, self.starts()), (2, []))
            (where / "cgroup.procs").write_text("")
        summary, _ = self.run_jobs([job], self.opts())
        self.assertEqual((summary.exit_code, self.starts()), (0, ["sha"]))

    def test_cgroup_that_will_not_empty_stops_the_run(self) -> None:
        """A member that cannot be killed (pid 1 stands in): the run stops rather than start the
        next job beside it."""
        spec = {"write": [{"path": str(self.cg / "cgroup.procs"), "text": "1\n"}]}
        jobs = [self.job("sha", [spec]), self.job("next", [{"stamp": str(self.stamps),
                                                            "name": "next"}])]
        with mock.patch.object(qor_sched, "CGROUP_DRAIN_S", 0.3):
            summary, lines = self.run_jobs(jobs, self.opts())
        error = f"P sha: failed (drain): {self.cg} still holds processes [1] after 0.3 s; stopping"
        self.assertIn(f"vtr-qor: {error}", lines)
        self.assertIn("vtr-qor: [1/2] P sha failed 0.0 s (A, j1)", lines)
        self.assertEqual([(r.job.design, r.status, r.error) for r in summary.results],
                         [("sha", "failed", error), ("next", "skipped", "not started")])
        self.assertEqual((self.job_dir("sha") / "status").read_text(), "failed\n")
        self.assertEqual((summary.exit_code, self.starts()), (2, []))

    def test_no_cgroup_no_job(self) -> None:
        """A job cgroup that cannot be created (here: a file in its place): nothing starts."""
        self.cg.write_text("")
        summary, lines = self.run_jobs([self.job("sha", [{"stamp": str(self.stamps),
                                                          "name": "sha"}])], self.opts())
        self.assertTrue(lines[0].startswith(f"vtr-qor: cannot create the job cgroup {self.cg}"))
        self.assertEqual((summary.exit_code, self.starts()), (2, []))


class DfTest(RunnerTestBase):
    """Review I2: a hung df of /mnt/c is bounded, and never holds up a job's wall timeout."""

    def test_df_hangs_between_jobs(self) -> None:
        self.df_hang.touch()
        began = time.monotonic()
        with mock.patch.object(qor_heavy, "DF_TIMEOUT_S", 0.3):
            summary, lines = self.run_jobs([self.job("sha", [{"stamp": str(self.stamps),
                                                              "name": "sha"}])], self.opts())
        self.assertLess(time.monotonic() - began, 60)  # the hung df sleeps 600 s
        self.assertEqual(lines[0], "vtr-qor: cannot read free space of /mnt/c")
        self.assertEqual((summary.exit_code, self.starts()), (5, []))

    def test_df_hangs_while_running(self) -> None:
        spec = {"write": [{"path": str(self.df_hang), "text": ""}], "sleep": 600}
        began = time.monotonic()
        with mock.patch.object(qor_heavy, "DF_TIMEOUT_S", 0.3):
            summary, _ = self.run_jobs([self.job("sha", [spec])],
                                       self.opts(clock=ScaledClock(600)))
        self.assertLess(time.monotonic() - began, 60)  # the hung df sleeps 600 s
        self.assertEqual((summary.results[0].status, summary.results[0].error, summary.exit_code),
                         ("aborted", "cannot read free space of /mnt/c", 5))

    def test_timeout_despite_hung_df(self) -> None:
        pids = self.tmp / "pids"
        spec = {"write": [{"path": str(self.df_hang), "text": ""}], "spawn_sleep": 600,
                "pids": str(pids), "sleep": 600}
        frozen = time.monotonic()
        opts = self.opts(clock=lambda: time.monotonic() if pids.exists() else frozen,
                         sleep=time.sleep, job_timeout_s={"A": 1, "B": 1})
        began = time.monotonic()
        with mock.patch.object(qor_sched, "POLL_S", 0.05), \
                mock.patch.object(qor_sched, "KILL_GRACE_S", 0.5):
            result = qor_sched.run_one(self.job("sha", [spec]), opts, finish_ok)
        self.assertEqual(result.status, "timeout")  # not aborted by the df probe's 30 s limit
        self.assertLess(time.monotonic() - began, 25)


class CompletionTest(RunnerTestBase):
    """Review I3: an ok metrics.json is written last, after cleanup and status; a job left
    half-done is redone."""

    def p_spec(self) -> Spec:
        run = self.job_dir("sha") / "run"
        return {"stamp": str(self.stamps), "name": "sha",
                "write": [{"path": str(run / "sha.pre-vpr.blif"), "text": ".model sha\n"}]}

    def test_interrupted_cleanup_publishes_nothing(self) -> None:
        job = self.job("sha", [self.p_spec()])
        with mock.patch.object(qor_sched, "cleanup_job", side_effect=SystemExit(143)), \
                self.assertRaises(SystemExit):
            qor_sched.run_one(job, self.opts(), finish_ok)
        self.assertFalse((job.job_dir / "metrics.json").exists())
        self.assertFalse(qor_sched.job_complete(job))

    def test_cleanup_failure_fails_the_job(self) -> None:
        job = self.job("sha", [self.p_spec()])
        with mock.patch.object(qor_sched, "cleanup_job",
                               side_effect=HeavyError(2, "refusing to delete x")):
            result = qor_sched.run_one(job, self.opts(), finish_ok)
        self.assertEqual((result.status, result.error),
                         ("failed", "P sha: cleanup failed: refusing to delete x"))
        self.assertEqual(json.loads((job.job_dir / "metrics.json").read_text())["status"],
                         "failed")
        self.assertFalse(qor_sched.job_complete(job))

    def test_order_cleanup_status_metrics(self) -> None:
        job = self.job("sha", [self.p_spec()])
        result = qor_sched.run_one(job, self.opts(), finish_ok)
        self.assertEqual(result.status, "ok")
        run = job.job_dir / "run"
        self.assertTrue((run / "sha.pre-vpr.blif.gz").exists())
        self.assertFalse((run / "sha.pre-vpr.blif").exists())
        self.assertLessEqual((job.job_dir / "status").stat().st_mtime_ns,
                             (job.job_dir / "metrics.json").stat().st_mtime_ns)
        self.assertTrue(qor_sched.job_complete(job))

    def test_half_done_job_redone(self) -> None:
        job = self.job("sha", [self.p_spec()], cached=True)
        job.job_dir.mkdir(parents=True)
        (job.job_dir / "metrics.json").write_text(json.dumps({"status": "ok", "key": job.key}))
        self.run_jobs([job], self.opts())  # no status: redone
        self.assertEqual(self.starts(), ["sha"])
        self.run_jobs([job], self.opts())  # complete now: cached
        self.assertEqual(self.starts(), ["sha"])
        (job.job_dir / "run" / "sha.pre-vpr.blif").write_text(".model sha\n")
        self.assertFalse(qor_sched.job_complete(job))  # P's netlist not gzipped: redone
        self.run_jobs([job], self.opts())
        self.assertEqual(self.starts(), ["sha", "sha"])

    def test_prepare_after_fresh_attempt(self) -> None:
        seen: list[list[str]] = []

        def prepare(job: Job, start: float) -> None:
            seen.append(sorted(p.name for p in job.job_dir.iterdir()))
            (job.job_dir / "job.json").write_text("{}\n")

        job = self.job("sha", [{"exit": 1}])
        job.job_dir.mkdir(parents=True)
        (job.job_dir / "old").write_text("x\n")
        result = qor_sched.run_one(job, self.opts(prepare=prepare), finish_ok)
        self.assertEqual((seen, result.status), ([["run"]], "failed"))
        self.assertTrue((job.job_dir / "job.json").exists())  # a failed job keeps job.json

        def broken(job: Job, start: float) -> None:
            raise ValueError("no disk")

        result = qor_sched.run_one(self.job("sha", [{"stamp": str(self.stamps), "name": "x"}]),
                                   self.opts(prepare=broken), finish_ok)
        self.assertEqual((result.status, result.error, self.starts()),
                         ("failed", "P sha: cannot prepare the job: no disk", []))

    def test_unwritable_log(self) -> None:
        (self.out / "harness.log").mkdir()
        summary, lines = self.run_jobs([self.job("sha", [{}])], self.opts())
        self.assertEqual(summary.exit_code, 0)
        self.assertEqual(sum("cannot write" in line for line in lines), 1)
        self.assertTrue(lines[-1].startswith("vtr-qor: done: 1 ok"))

    def test_default_clock_is_monotonic(self) -> None:
        self.assertIs(qor_sched.RunOptions(out=self.out).clock, time.monotonic)


class AdmissionTest(RunnerTestBase):
    def test_admission_disk_pause_stop_floor(self) -> None:
        # Paused at 15 GiB, resumed at 25: the job then runs.
        self.set_df(15)
        clock = FakeClock()
        clock.on_sleep = lambda n: self.set_df(25)
        job = self.job("sha", [{"stamp": str(self.stamps), "name": "sha"}])
        summary, lines = self.run_jobs([job], self.opts(clock=clock, sleep=clock.sleep))
        self.assertEqual(lines[:2], [
            "vtr-qor: paused: C: has 15 GiB free (< 20 GiB); checking every 60 s",
            "vtr-qor: resumed: C: has 25 GiB free"])
        self.assertEqual(clock.sleeps, [60])
        self.assertEqual((self.starts(), summary.exit_code), (["sha"], 0))

    def test_admission_pause_limit(self) -> None:
        self.set_df(15)
        clock = FakeClock()
        jobs = [self.job(d, [{"stamp": str(self.stamps), "name": d}]) for d in ("a", "b", "c")]
        summary, lines = self.run_jobs(jobs, self.opts(clock=clock, sleep=clock.sleep))
        self.assertEqual(lines[:3], [
            "vtr-qor: paused: C: has 15 GiB free (< 20 GiB); checking every 60 s",
            "vtr-qor: C: stayed below 20 GiB free for 2 h; stopping (3 jobs not started)",
            HINT])
        self.assertEqual(sum(clock.sleeps), 7200)
        self.assertEqual(self.starts(), [])
        self.assertEqual([r.status for r in summary.results], ["skipped"] * 3)
        self.assertEqual((summary.exit_code, summary.stopped_for_disk), (5, True))
        self.assertTrue(lines[-1].startswith("vtr-qor: done: 0 ok, 0 failed, 0 capped, "
                                             "0 timeout, 3 skipped in "))

    def test_admission_floor_while_running(self) -> None:
        """C: drops to 9 GiB while a job runs: it is terminated (aborted), the run stops, exit 5."""
        pids = self.tmp / "pids"
        spec = {"write": [{"path": str(self.df_file), "text": "9\n"}], "spawn_sleep": 600,
                "pids": str(pids), "sleep": 600}
        jobs = [self.job("sha", [spec]),
                self.job("later", [{"stamp": str(self.stamps), "name": "later"}])]
        summary, lines = self.run_jobs(jobs, self.opts(clock=ScaledClock(600)))
        self.assertIn("vtr-qor: C: fell below 10 GiB free; terminating 1 running jobs", lines)
        self.assertEqual([r.status for r in summary.results], ["aborted", "skipped"])
        self.assertEqual((summary.exit_code, summary.stopped_for_disk), (5, True))
        self.assertEqual(self.starts(), [])
        self.assertTrue(_wait_dead([int(p) for p in pids.read_text().split()]))
        self.assertEqual((self.job_dir("sha") / "status").read_text(), "aborted\n")
        self.assertIn(", 1 aborted in ", lines[-1])

    def test_df_fails_while_running(self) -> None:
        spec = {"write": [{"path": str(self.df_file), "text": "garbage\n"}], "sleep": 600}
        summary, lines = self.run_jobs([self.job("sha", [spec])],
                                       self.opts(clock=ScaledClock(600)))
        self.assertEqual(summary.results[0].error, "cannot read free space of /mnt/c")
        self.assertEqual((summary.results[0].status, summary.exit_code), ("aborted", 5))

    def test_df_fails_between_jobs(self) -> None:
        self.df_file.write_text("garbage\n")
        summary, lines = self.run_jobs([self.job("sha", [{}])], self.opts())
        self.assertEqual(lines[0], "vtr-qor: cannot read free space of /mnt/c")
        self.assertEqual(([r.status for r in summary.results], summary.exit_code),
                         (["skipped"], 5))

    def test_admission_mem_wait(self) -> None:
        self.set_mem(6.5)
        clock = FakeClock()
        clock.on_sleep = lambda n: self.set_mem(8.0) if n == 3 else None
        job = self.job("sha", [{"stamp": str(self.stamps), "name": "sha"}])
        summary, lines = self.run_jobs([job], self.opts(clock=clock, sleep=clock.sleep))
        self.assertEqual(lines[0], "vtr-qor: waiting: 6.5 GiB available (< 7)")
        self.assertEqual(lines[1], "vtr-qor: [1/1] P sha ok 0.0 s (A, j1)")
        self.assertEqual(clock.sleeps, [60, 60, 60])
        self.assertEqual(self.starts(), ["sha"])
        opts = self.opts()
        self.set_mem(6.99)
        with redirect_stderr(io.StringIO()) as err:
            self.assertEqual(qor_sched.admit(opts, running=0), "wait")
        self.assertEqual(err.getvalue(), "vtr-qor: waiting: 6.9 GiB available (< 7)\n")
        self.set_mem(7.0)
        self.assertEqual(qor_sched.admit(opts, running=0), "go")

    def test_admission_mem_limit(self) -> None:
        """MemAvailable short for PAUSE_LIMIT_S without a break stops the run (exit 5, no
        compaction hint); a recovery in between starts the count again."""
        self.set_mem(6.5)
        clock = FakeClock()
        jobs = [self.job(d, [{"stamp": str(self.stamps), "name": d}]) for d in ("a", "b")]
        summary, lines = self.run_jobs(jobs, self.opts(clock=clock, sleep=clock.sleep))
        self.assertEqual(lines[:3], [
            "vtr-qor: waiting: 6.5 GiB available (< 7)",
            "vtr-qor: MemAvailable stayed below 7 GiB for 2 h; stopping (2 jobs not started)",
            "vtr-qor: done: 0 ok, 0 failed, 0 capped, 0 timeout, 2 skipped in 2:00:00"])
        self.assertEqual((sum(clock.sleeps), summary.exit_code, self.starts()), (7200, 5, []))
        # Short for 1 h, fine, short again for 1 h: the count restarted, so no stop.
        clock = FakeClock()
        clock.on_sleep = lambda n: self.set_mem(8.0 if n == 60 else 6.5)
        summary, lines = self.run_jobs(jobs, self.opts(clock=clock, sleep=clock.sleep))
        self.assertEqual((summary.exit_code, self.starts()), (0, ["a", "b"]))

    def test_admit_running(self) -> None:
        """With a job running only the floor counts: 15 GiB goes on, 9 stops; memory is not
        read."""
        opts = self.opts()
        self.set_mem(1.0)
        self.set_df(15)
        self.assertEqual(qor_sched.admit(opts, running=1), "go")
        self.set_df(10)
        self.assertEqual(qor_sched.admit(opts, running=1), "go")
        self.set_df(9)
        self.assertEqual(qor_sched.admit(opts, running=1), "stop")
        self.set_df(20)
        with redirect_stderr(io.StringIO()):
            self.assertEqual(qor_sched.admit(opts, running=0), "wait")  # memory, not disk
        self.set_mem(16.0)
        self.assertEqual(qor_sched.admit(opts, running=0), "go")  # 20 GiB is enough
        self.set_df(19)
        with redirect_stderr(io.StringIO()) as err:
            self.assertEqual(qor_sched.admit(opts, running=0), "wait")
        self.assertIn("paused: C: has 19 GiB free", err.getvalue())

    def test_deadline(self) -> None:
        clock = FakeClock()
        jobs = [self.job(d, [{"stamp": str(self.stamps), "name": d}]) for d in ("a", "b", "c")]
        minutes = {"a": 30, "b": 200, "c": 30}
        opts = self.opts(clock=clock, sleep=clock.sleep, deadline=clock.now + 100 * 60,
                         expected_s=lambda job: minutes[job.design] * 60.0)
        summary, lines = self.run_jobs(jobs, opts)
        self.assertEqual([r.status for r in summary.results], ["ok", "skipped", "ok"])
        # The deadline is on the run's clock; the text shows it as local wall time.
        whens = {time.strftime("%H:%M", time.localtime(time.time() + 100 * 60 + d))
                 for d in (-60, 0)}
        self.assertTrue(any(f"vtr-qor: nightly: P b not started: expected 200 min would pass "
                            f"{when}" in lines for when in whens), lines)
        self.assertEqual(summary.exit_code, 0)


class CleanupTest(RunnerTestBase):
    def ok(self, job: Job) -> qor_sched.JobResult:
        return qor_sched.JobResult(job, "ok", 1.0, "")

    def test_cleanup_keep_list(self) -> None:
        job = self.job("sha", [])
        run = job.job_dir / "run"
        run.mkdir(parents=True)
        for name in KEEP_PRESENT + SCRATCH:
            (run / name).write_text(f"{name} contents\n" * 50)
        self.assertEqual(len(list(run.iterdir())), 20)
        original = (run / "sha.pre-vpr.blif").read_bytes()
        qor_sched.cleanup_job(job, self.ok(job), frozenset())
        self.assertEqual(sorted(p.name for p in run.iterdir()),
                         sorted((*KEEP_PRESENT, "sha.pre-vpr.blif.gz")))
        self.assertEqual(gzip.decompress((run / "sha.pre-vpr.blif.gz").read_bytes()), original)
        # Reproducible bytes (no name, no time in the gzip header).
        first = (run / "sha.pre-vpr.blif.gz").read_bytes()
        (run / "sha.pre-vpr.blif").write_bytes(original)
        qor_sched.cleanup_job(job, self.ok(job), frozenset())
        self.assertEqual((run / "sha.pre-vpr.blif.gz").read_bytes(), first)

    def make_o(self, leg: str = "O") -> tuple[Job, Path, Path]:
        job = self.job("sha", [], leg=leg)
        qor_heavy.safe_rmtree(job.job_dir, self.out)
        run = job.job_dir / "run"
        (run / "sub").mkdir(parents=True)
        (run / "sub" / "x").write_text("x\n")
        for name in ("vpr.out", "sha.pre-vpr.blif", "sha.net"):
            (run / name).write_text(name)
        blif = job.job_dir / "sha.blif"
        blif.write_text(".model sha\n.end\n")
        return job, run, blif

    def test_cleanup_netlists(self) -> None:
        job, run, blif = self.make_o()
        digest = hashlib.sha256(blif.read_bytes()).hexdigest()
        qor_sched.cleanup_job(job, self.ok(job), frozenset({"Oprime"}))
        self.assertEqual(sorted(p.name for p in run.iterdir()), ["vpr.out"])
        self.assertFalse(blif.exists())
        self.assertEqual((job.job_dir / "sha.blif.sha256").read_text(), digest + "\n")
        job, run, blif = self.make_o()
        qor_sched.cleanup_job(job, self.ok(job), frozenset({"O"}))
        self.assertEqual(sorted(p.name for p in run.iterdir()), ["sha.pre-vpr.blif", "vpr.out"])
        self.assertTrue(blif.exists())
        self.assertFalse((job.job_dir / "sha.blif.sha256").exists())

    def test_cleanup_keeps_failed(self) -> None:
        for status in ("failed", "capped", "timeout", "aborted"):
            job, run, blif = self.make_o()
            qor_sched.cleanup_job(job, qor_sched.JobResult(job, status, 1.0, "x"), frozenset())
            self.assertEqual(sorted(p.name for p in run.iterdir()),
                             ["sha.net", "sha.pre-vpr.blif", "sub", "vpr.out"], status)
            self.assertTrue(blif.exists())

    def test_cleanup_refuses_symlink(self) -> None:
        job, run, _ = self.make_o()
        outside = self.tmp / "outside"
        outside.mkdir()
        (outside / "keep").write_text("keep\n")
        (run / "link").symlink_to(outside)
        with self.assertRaises(HeavyError) as caught:
            qor_sched.cleanup_job(job, self.ok(job), frozenset())
        self.assertEqual(caught.exception.code, 2)
        self.assertTrue((outside / "keep").exists())

    def test_run_cleans_ok_job(self) -> None:
        run = self.job_dir("sha") / "run"
        spec = {"write": [{"path": str(run / n), "text": n} for n in ("vpr.out", "sha.net",
                                                                      "sha.pre-vpr.blif")]}
        summary, _ = self.run_jobs([self.job("sha", [spec])], self.opts())
        self.assertEqual(summary.exit_code, 0)
        self.assertEqual(sorted(p.name for p in run.iterdir()),
                         ["sha.pre-vpr.blif.gz", "vpr.out", "vtr_flow.out"])


class RetentionBase(RunnerTestBase):
    """Retention fixtures: a P leg directory with keys and jobs made by hand."""

    def leg_dir(self, leg: str = "P") -> Path:
        return self.out / VTR12 / K6 / leg

    def make(self, key12: str, design: str, status: str, when: float, *,
             size: int = 100) -> Path:
        job_dir = self.leg_dir() / key12 / design
        (job_dir / "run").mkdir(parents=True)
        with (job_dir / "run" / "big").open("wb") as handle:
            handle.truncate(size)
        if status == "ok":
            (job_dir / "metrics.json").write_text(json.dumps({"status": "ok", "key": "k"}))
        (job_dir / "status").write_text(status + "\n")
        os.utime(job_dir / "status", (when, when))
        return job_dir

    def prune(self, designs: tuple[str, ...] | None = ("sha", "ch_intrinsics"),
              protect: tuple[str, ...] = ()) -> None:
        qor_sched.prune(self.out, VTR12, K6, "P", designs=designs, protect=protect)

    def names(self) -> list[str]:
        return sorted(p.name for p in self.leg_dir().iterdir() if p.is_dir())


class RetentionTest(RetentionBase):
    def test_retention_keys(self) -> None:
        keys = [f"{i:012x}" for i in range(5)]
        for i, key in enumerate(keys):
            for design in ("sha", "ch_intrinsics"):
                self.make(key, design, "ok", 1_000_000.0 + i)
        incomplete = self.make("incomplete00", "sha", "ok", 1.0)
        self.make("incomplete00", "ch_intrinsics", "failed", 2.0)
        self.prune(designs=None)  # no set: no key is pruned
        self.assertEqual(self.names(), sorted([*keys, "incomplete00"]))
        self.prune()  # the old incomplete key goes too: 3 complete keys are newer (M7)
        self.assertEqual(self.names(), keys[2:])
        self.assertFalse(incomplete.exists())
        # latest is never pruned, even when older keys would be.
        (self.leg_dir() / "latest").write_text(keys[2] + "\n")
        for i in range(5, 8):
            for design in ("sha", "ch_intrinsics"):
                self.make(f"{i:012x}", design, "ok", 1_000_000.0 + i)
        self.prune()
        self.assertEqual(self.names(), sorted([keys[2], *(f"{i:012x}" for i in range(5, 8))]))

    def test_retention_complete_means_the_whole_set(self) -> None:
        """Point (f): a key holding only some designs of the set (a smoke or seed run) is not
        complete, so it neither counts among the kept keys nor is pruned."""
        self.make("000000000000", "sha", "ok", 1.0)
        self.make("000000000000", "ch_intrinsics", "ok", 1.0)
        for i in range(1, 5):
            self.make(f"{i:012x}", "sha", "ok", 1_000_000.0 + i)  # partial: sha only
        self.prune()
        self.assertEqual(len(self.names()), 5)
        # A complete job needs status ok, an ok metrics.json and (P) the netlist gzipped.
        for i in range(1, 4):
            self.make(f"{i:012x}", "ch_intrinsics", "ok", 1_000_000.0 + i)
        # Every design of the set ok, but another job of the key failed: not complete either.
        self.make("000000000001", "extra", "failed", 1.0)
        raw = self.make("000000000004", "ch_intrinsics", "ok", 1_000_004.0) / "run"
        (raw / "ch_intrinsics.pre-vpr.blif").write_text(".model x\n")
        self.prune()  # complete: 0, 2, 3 only (1 has a failed job, 4 an un-gzipped netlist)
        self.assertEqual(self.names(), [f"{i:012x}" for i in range(5)])
        (raw / "ch_intrinsics.pre-vpr.blif").unlink()  # 4 complete now: 0 is the 4th newest,
        self.prune()  # and 1 (incomplete) is older than 3 complete keys (M7)
        self.assertEqual(self.names(), [f"{i:012x}" for i in range(2, 5)])

    def test_retention_old_incomplete_keys(self) -> None:
        """An incomplete key goes once 3 complete keys are newer than it, unless it is latest
        or the run's own."""
        for i in range(1, 4):
            for design in ("sha", "ch_intrinsics"):
                self.make(f"{i:012x}", design, "ok", 1_000_000.0 + i)
        self.make("aaaaaaaaaaaa", "sha", "failed", 1.0)       # older than all 3: goes
        self.make("bbbbbbbbbbbb", "sha", "ok", 1_000_002.5)   # newer than one: stays
        self.make("cccccccccccc", "sha", "failed", 2.0)       # old but latest: stays
        self.make("dddddddddddd", "sha", "failed", 3.0)       # old but the run's: stays
        (self.leg_dir() / "latest").write_text("cccccccccccc\n")
        self.prune(protect=("dddddddddddd",))
        self.assertEqual(self.names(), ["000000000001", "000000000002", "000000000003",
                                        "bbbbbbbbbbbb", "cccccccccccc", "dddddddddddd"])
        # With only 2 newer complete keys, an old incomplete key stays.
        qor_heavy.safe_rmtree(self.leg_dir() / "000000000003", self.out)
        self.make("eeeeeeeeeeee", "sha", "failed", 1.0)
        self.prune()
        self.assertIn("eeeeeeeeeeee", self.names())

    def test_retention_protects_the_run(self) -> None:
        for i in range(5):
            for design in ("sha", "ch_intrinsics"):
                self.make(f"{i:012x}", design, "ok", 1_000_000.0 + i)
        self.prune(protect=("000000000000",))
        self.assertEqual(self.names(), ["000000000000", "000000000002", "000000000003",
                                        "000000000004"])

    def test_retention_failed_count(self) -> None:
        dirs = [self.make("aaaaaaaaaaaa", f"d{i}", "failed" if i % 2 else "capped",
                          1_000_000.0 + 10 * (7 - i)) for i in range(7)]
        self.prune()
        with_run = [d for d in dirs if (d / "run").exists()]
        self.assertEqual(with_run, dirs[:5])
        self.assertTrue(all((d / "status").exists() for d in dirs))

    def test_retention_failed_bytes(self) -> None:
        dirs = [self.make("bbbbbbbbbbbb", f"d{i}", "timeout", 1_000_000.0 + i, size=GIB)
                for i in range(3)]
        self.prune()
        self.assertEqual([(d / "run").exists() for d in dirs], [False, True, True])
        total = sum(qor_sched._tree_bytes(d / "run") for d in dirs if (d / "run").exists())
        self.assertLessEqual(total, 2 * GIB)

    def test_retention_refuses_symlinks(self) -> None:
        outside = self.tmp / "outside"
        (outside / "sha").mkdir(parents=True)
        (outside / "sha" / "status").write_text("ok\n")
        self.make("cccccccccccc", "sha", "ok", 1.0)
        link = self.leg_dir() / "dddddddddddd"
        link.symlink_to(outside)
        with self.assertRaises(HeavyError) as caught:
            self.prune()
        self.assertEqual((caught.exception.code, caught.exception.message),
                         (2, f"refusing to delete {link}: {link} is a symlink"))
        self.assertTrue((outside / "sha" / "status").exists())
        link.unlink()
        (self.leg_dir() / "cccccccccccc" / "lnk").symlink_to(outside)
        with self.assertRaises(HeavyError):
            self.prune()
        (self.leg_dir() / "cccccccccccc" / "lnk").unlink()
        status = self.leg_dir() / "cccccccccccc" / "sha" / "status"
        status.rename(self.tmp / "status")
        status.symlink_to(self.tmp / "status")
        with self.assertRaises(HeavyError):
            self.prune()
        status.unlink()
        (self.tmp / "status").rename(status)
        (self.leg_dir() / "latest").symlink_to(self.tmp / "status")
        with self.assertRaises(HeavyError):
            self.prune()
        (self.leg_dir() / "latest").unlink()
        moved = self.tmp / "realleg"
        self.leg_dir().rename(moved)
        self.leg_dir().symlink_to(moved)
        with self.assertRaises(HeavyError):
            self.prune()
        self.assertTrue((moved / "cccccccccccc" / "sha" / "status").exists())

    def test_run_prunes_after(self) -> None:
        for i in range(4):
            self.make(f"{i:012x}", "sha", "ok", 1_000_000.0 + i)
        self.run_jobs([self.job("sha", [{}])], self.opts())
        self.assertEqual(len(self.names()), 5)  # no retain_designs: no key pruned
        self.run_jobs([self.job("sha", [{}])], self.opts(retain_designs=frozenset({"sha"})))
        self.assertEqual(self.names(), ["000000000002", "000000000003", KEY12])

    def test_run_keeps_its_own_cached_key(self) -> None:
        """A run whose job is cached (old) never loses its own key to newer complete keys."""
        job = self.job("sha", [{"stamp": str(self.stamps), "name": "sha"}], cached=True)
        own = self.make(KEY12, "sha", "ok", 1.0)
        (own / "metrics.json").write_text(json.dumps({"status": "ok", "key": job.key}))
        for i in range(3):
            self.make(f"{i:012x}", "sha", "ok", 1_000_000.0 + i)
        summary, _ = self.run_jobs([job], self.opts(retain_designs=frozenset({"sha"})))
        self.assertEqual(([r.status for r in summary.results], self.starts()), (["ok"], []))
        self.assertTrue(own.exists())


class ProgressTest(RunnerTestBase):
    def test_progress_lines(self) -> None:
        log = qor_sched.HarnessLog(self.out)
        job = self.job("sha", [])
        results = ([qor_sched.JobResult(job, "ok", 14.25, "")] * 19
                   + [qor_sched.JobResult(job, "failed", 1.0, "x"),
                      qor_sched.JobResult(job, "timeout", 1.0, "x"),
                      qor_sched.JobResult(dataclasses.replace(job, group="B", leg="O"),
                                          "capped", 3723.04, "x")])
        with redirect_stderr(io.StringIO()) as err:
            log.job(3, 21, qor_sched.JobResult(job, "ok", 14.21, ""))
            log.job(21, 21, results[-1])
            log.done(results, 3723.4)
        expected = ["vtr-qor: [3/21] P sha ok 14.2 s (A, j1)",
                    "vtr-qor: [21/21] O sha capped 3723.0 s (B, j1)",
                    "vtr-qor: done: 19 ok, 1 failed, 1 capped, 1 timeout, 0 skipped in 1:02:03"]
        self.assertEqual(err.getvalue().splitlines(), expected)
        self.assertEqual((self.out / "harness.log").read_text().splitlines(), expected)
        self.assertEqual((qor_sched.hms(0.6), qor_sched.hms(59.5), qor_sched.hms(-3)),
                         ("0:00:01", "0:01:00", "0:00:00"))
        with redirect_stderr(io.StringIO()):
            log.done([], 0.4)
        self.assertEqual((self.out / "harness.log").read_text().splitlines()[-1],
                         "vtr-qor: done: 0 ok, 0 failed, 0 capped, 0 timeout, 0 skipped in 0:00:00")

    def test_run_lines(self) -> None:
        jobs = [self.job("sha", [{}]), self.job("mcml", [{"exit": 2}], group="B")]
        clock = FakeClock()  # elapsed times are pinned by test_progress_lines
        summary, lines = self.run_jobs(jobs, self.opts(clock=clock), starts=True)
        flow = self.job_dir("mcml") / "run" / "vtr_flow.out"
        python = Path(sys.executable).name
        self.assertEqual(lines, [
            "vtr-qor: [1/2] P sha start",
            "vtr-qor: [1/2] P sha ok 0.0 s (A, j1)",
            "vtr-qor: [2/2] P mcml start",
            f"vtr-qor: P mcml: {python} exited 2; see {flow}",
            "vtr-qor: [2/2] P mcml failed 0.0 s (B, j1)",
            "vtr-qor: done: 1 ok, 1 failed, 0 capped, 0 timeout, 0 skipped in 0:00:00"])
        self.assertEqual(lines, (self.out / "harness.log").read_text().splitlines())


class RetentionFailedOnlyTest(RetentionBase):
    def test_retention_clears_only_failed_run_dirs(self) -> None:
        """Retention thins failed jobs' run directories only: a complete key of six ok P jobs
        (older than every failed one, so they would go first) keeps every parse file and
        gzipped netlist while seven failed jobs are cut to KEEP_FAILED run directories."""
        designs = ("ch_intrinsics", "stereovision3", "diffeq2", "diffeq1", "mkPktMerge", "sha")
        kept = ("parse_results.txt", "qor_results.txt", "vpr.out", "vpr.crit_path.out",
                "parmys.out", "abc0.out")
        ok_files = {}
        for i, design in enumerate(designs):
            job = self.make("aaaaaaaaaaaa", design, "ok", 1000.0 + i)
            for name in (*kept, f"{design}.pre-vpr.blif.gz"):
                (job / "run" / name).write_text(f"{design} {name}\n")
            ok_files[design] = sorted(p.name for p in (job / "run").iterdir())
        (self.leg_dir() / "aaaaaaaaaaaa" / "planned.json").write_text(
            json.dumps({"designs": sorted(designs)}))
        failed = [self.make("bbbbbbbbbbbb", f"f{i}", "failed", 2000.0 + i) for i in range(7)]
        qor_sched.prune(self.out, VTR12, K6, "P", planned=True)
        for design in designs:
            run = self.leg_dir() / "aaaaaaaaaaaa" / design / "run"
            self.assertEqual(sorted(p.name for p in run.iterdir()), ok_files[design], design)
        self.assertEqual([(job / "run").is_dir() for job in failed],
                         [False, False, True, True, True, True, True])
        self.assertTrue(all((job / "status").read_text() == "failed\n" for job in failed))


class SignalTest(unittest.TestCase):
    """Final review I1: an inherited SIG_IGN is left alone; any other disposition is replaced
    by SystemExit(128 + N) while jobs run and restored afterwards."""

    def test_signals_as_exit(self) -> None:
        def noop(signum: int, frame: object) -> None:
            return None

        before_term = signal.signal(signal.SIGTERM, noop)
        before_hup = signal.signal(signal.SIGHUP, signal.SIG_IGN)
        try:
            with qor_sched._signals_as_exit():
                self.assertIs(signal.getsignal(signal.SIGHUP), signal.SIG_IGN)
                self.assertIs(signal.getsignal(signal.SIGTERM), qor_sched._raise_exit)
                os.kill(os.getpid(), signal.SIGHUP)  # ignored: nothing happens
                with self.assertRaises(SystemExit) as ctx:
                    os.kill(os.getpid(), signal.SIGTERM)
                    time.sleep(1)
                self.assertEqual(ctx.exception.code, 143)
            self.assertIs(signal.getsignal(signal.SIGHUP), signal.SIG_IGN)
            self.assertIs(signal.getsignal(signal.SIGTERM), noop)
            signal.signal(signal.SIGHUP, noop)
            with qor_sched._signals_as_exit(), self.assertRaises(SystemExit) as ctx:
                os.kill(os.getpid(), signal.SIGHUP)
                time.sleep(1)
            self.assertEqual(ctx.exception.code, 129)
        finally:
            signal.signal(signal.SIGTERM, before_term)
            signal.signal(signal.SIGHUP, before_hup)


class FixRound6Test(RetentionBase):
    """Task 6 fix round 1: planned.json, the planned-set retention, after_run, DrainError."""

    def test_planned_written_before_jobs(self) -> None:
        seen: list[object] = []

        def finish(job: Job, start: float) -> dict[str, object]:
            seen.append(qor_sched.read_planned(job.job_dir.parent))
            return finish_ok(job, start)

        jobs = [self.job("sha", [{}]), self.job("ch_intrinsics", [{}])]
        summary, _ = self.run_jobs(jobs, self.opts(), finish)
        self.assertEqual(summary.exit_code, 0)
        both = frozenset({"sha", "ch_intrinsics"})
        self.assertEqual(seen, [both, both])
        self.run_jobs([self.job("mcml", [{}], group="B")], self.opts())
        self.assertEqual(qor_sched.read_planned(self.leg_dir() / KEY12), both | {"mcml"})

    def test_growing_planned_set_retracts_latest(self) -> None:
        """Review I3: latest naming a key is retracted before that key's planned set grows,
        and kept when the set does not grow."""
        key = self.leg_dir() / KEY12
        key.mkdir(parents=True)
        (key / "planned.json").write_text('{"designs": ["sha"]}')
        latest = self.leg_dir() / "latest"
        latest.write_text(f"{KEY12}\n")
        self.assertEqual(qor_sched.write_planned([self.job("sha", [{}])]), [])
        self.assertTrue(latest.exists())
        self.assertEqual(qor_sched.write_planned([self.job("mcml", [{}], group="B")]),
                         [f"P {KEY12}"])
        self.assertFalse(latest.exists())
        self.assertEqual(qor_sched.read_planned(key), frozenset({"sha", "mcml"}))
        latest.write_text("aaaaaaaaaaaa\n")  # another key's latest is left alone
        self.assertEqual(qor_sched.write_planned([self.job("ch_intrinsics", [{}])]), [])
        self.assertEqual(latest.read_text(), "aaaaaaaaaaaa\n")

    def test_retract_latest(self) -> None:
        """Only a latest naming the job's own key goes; a symlinked latest is refused."""
        latest = self.leg_dir() / "latest"
        latest.parent.mkdir(parents=True)
        job = self.job("sha", [{}])
        latest.write_text("aaaaaaaaaaaa\n")
        self.assertFalse(qor_sched.retract_latest(job))
        self.assertTrue(latest.exists())
        latest.write_text(f"{KEY12}\n")
        self.assertTrue(qor_sched.retract_latest(job))
        self.assertFalse(latest.exists())
        latest.symlink_to(self.tmp / "x")
        with self.assertRaisesRegex(HeavyError, "is a symlink"):
            qor_sched.retract_latest(job)

    def test_read_planned_refusals(self) -> None:
        key = self.leg_dir() / "aaaaaaaaaaaa"
        key.mkdir(parents=True)
        self.assertIsNone(qor_sched.read_planned(key))
        for bad in ("[]", '{"designs": "sha"}', '{"designs": ["a/b"]}', "nope"):
            (key / "planned.json").write_text(bad)
            self.assertIsNone(qor_sched.read_planned(key), bad)
        (key / "planned.json").unlink()
        (key / "planned.json").symlink_to(self.tmp / "x")
        with self.assertRaisesRegex(HeavyError, "is a symlink"):
            qor_sched.read_planned(key)

    def test_prune_by_own_planned_set(self) -> None:
        """Each key is complete by its own planned.json; a key without one never is."""
        for i, key in enumerate(("aaaaaaaaaaaa", "bbbbbbbbbbbb", "cccccccccccc",
                                 "dddddddddddd")):
            self.make(key, "sha", "ok", 1000.0 + i)
            (self.leg_dir() / key / "planned.json").write_text('{"designs": ["sha"]}')
        self.make("eeeeeeeeeeee", "sha", "ok", 2000.0)  # complete jobs, but no planned set
        self.make("ffffffffffff", "sha", "ok", 900.0)
        (self.leg_dir() / "ffffffffffff" / "planned.json").write_text(
            '{"designs": ["sha", "mcml"]}')  # planned mcml never ran: incomplete, old
        qor_sched.prune(self.out, VTR12, K6, "P", planned=True)
        self.assertEqual(self.names(), ["bbbbbbbbbbbb", "cccccccccccc", "dddddddddddd",
                                        "eeeeeeeeeeee"])

    def test_after_run_before_done(self) -> None:
        got: list[list[str]] = []

        def after(results: object) -> None:
            assert isinstance(results, list | tuple)
            got.append([r.status for r in results])
            qor_sched._emit(opts, "after")
            raise HeavyError(2, "after failed")

        opts = self.opts(after_run=after)
        summary, lines = self.run_jobs([self.job("sha", [{}])], opts)
        self.assertEqual((summary.exit_code, got), (0, [["ok"]]))
        self.assertEqual(lines[-3:-1], ["vtr-qor: after", "vtr-qor: after failed"])
        self.assertTrue(lines[-1].startswith("vtr-qor: done: 1 ok"))

    def test_drain_error_in_finish_stops_the_run(self) -> None:
        """Review M4: a busy cgroup left by a command finish ran stops the run at once; the
        next job is not started (so its directory is not wiped)."""
        def finish(job: Job, start: float) -> dict[str, object]:
            raise qor_sched.DrainError(2, "cg still holds processes [1] after 60 s; stopping")

        other = self.job("ch_intrinsics", [{"stamp": str(self.stamps), "name": "ch"}])
        summary, lines = self.run_jobs([self.job("sha", [{}]), other], self.opts(), finish)
        self.assertEqual(summary.exit_code, 2)
        self.assertEqual([r.status for r in summary.results], ["failed", "skipped"])
        self.assertIn("failed (drain)", summary.results[0].error)
        self.assertEqual(self.starts(), [])


if __name__ == "__main__":
    unittest.main()

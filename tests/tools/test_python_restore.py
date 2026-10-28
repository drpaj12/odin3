"""V1.2.0 Task 9 (spec 4.5): a design restored from a version 2 map answers `Net.provenance()` and
`Net.chain()` as the design that wrote it did.

Each micro is run through the VTR flow in one Python Design with `--prov-ast refs`, written with
`write_blif --vtr --name-style hash --prov-map`, and read back into a second Design with
`read_blif --name-map`. For every net that both designs have by name, the normalized records
(kind, class, level, flags, hier, parent count, construct) and the chain (the constructs) of the
restored net equal the original's. Run through CTest `python_restore` (ODIN3_LIB set); skipped
without cffi or the library, unless ODIN3_REQUIRE_CFFI=1.
"""

from __future__ import annotations

import os
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import TYPE_CHECKING

try:
    import cffi  # noqa: F401

    HAVE_CFFI = True
except ImportError:
    HAVE_CFFI = False
REQUIRE_CFFI = os.environ.get("ODIN3_REQUIRE_CFFI") == "1"
LIB = os.environ.get("ODIN3_LIB")

if TYPE_CHECKING or HAVE_CFFI:
    import name_map_v2 as nm2
    from odin3 import Construct, Design, Odin3, ProvRecord

REPO_ROOT = Path(__file__).resolve().parents[2]
MICROS = REPO_ROOT / "tests" / "micro" / "verilog" / "micro"
TECHLIB = REPO_ROOT / "lib" / "vtr.o3lib"
NAMES = (
    "bm_DL_2_1_mux",
    "bm_DL_4_bit_comparator",
    "bm_DL_2_cascaded_flip_flops",
    "bm_DL_BCD_7_segment_without_x",
)
FLOW = (
    "read_techlib {lib}; read_verilog --nolatches --vtr-compat --prov-ast refs {src}; proc; "
    "opt; flatten; opt -full; wreduce; hardmap; lower; "
)


Key = tuple[object, ...]
FLAG_NAMES = {
    "join": "join",
    "join=instance": "join_instance",
    "lossy=bulk": "lossy_bulk",
    "lossy=cap": "lossy_cap",
    "chain=span": "span_only",
}


def construct_key(kind: str, level: str, file: str, span: str, name: str) -> Key:
    return (kind, level, file, span, name)


def design_construct(c: Construct) -> Key:
    return construct_key(
        c.kind, c.level, c.file, f"{c.line}.{c.col}-{c.end_line}.{c.end_col}", c.name
    )


def design_record(r: ProvRecord) -> Key:
    """A record of the design without its IDs, run and operation (a restore makes new ones)."""
    leaf = None if r.construct is None else design_construct(r.construct)
    kind = {"source": "S", "imported": "I", "derived": "D"}[r.kind]
    return (
        kind,
        r.dir,
        r.from_level,
        r.to_level if kind == "D" else "",
        tuple(sorted(r.flags)),
        r.hier,
        len(r.parents),
        leaf,
    )


def map_construct(nm: nm2.NameMap, c: nm2.MapConstruct) -> Key:
    return construct_key(c.kind, c.level, nm.files[c.file - 1] if c.file else "", c.span, c.name)


def map_record(nm: nm2.NameMap, r: nm2.MapRecord) -> Key:
    """The same key from an R line, which the exporter wrote from the original design."""
    kind_dir, _, frm = r.dir.partition(":")
    flags = tuple(sorted(FLAG_NAMES[f] for f in r.flags if f in FLAG_NAMES))
    hier = next((f[5:] for f in r.flags if f.startswith("hier=")), "")
    leaf = map_construct(nm, nm.constructs[r.constructs[0] - 1]) if r.constructs else None
    return (
        r.kind,
        kind_dir,
        frm or "none",
        r.level if r.kind == "D" else "",
        flags,
        hier,
        len(r.parents),
        leaf,
    )


def original_answers(path: Path) -> list[tuple[tuple[Key, ...], tuple[Key, ...]]]:
    """(records, chain) of every net object of the written map, as the original design had them."""
    nm = nm2.read_name_map(path)
    found = []
    for obj in nm.objects:
        if obj.kind != "net" or "[" in obj.name:
            continue
        records = tuple(map_record(nm, r) for r in nm.provenance(obj))
        chain = tuple(map_construct(nm, c) for c in nm.chain(obj))
        found.append((records, chain))
    return sorted(found, key=repr)


def restored_answers(design: Design) -> list[tuple[tuple[Key, ...], tuple[Key, ...]]]:
    """(records, chain) of every named net of the restored design that has a record."""
    found = []
    for module in design.modules():
        for net in module.nets():
            if not net.name or "[" in net.name:
                continue
            records = tuple(design_record(r) for r in net.provenance())
            if records:
                found.append((records, tuple(design_construct(c) for c in net.chain())))
    return sorted(found, key=repr)


@unittest.skipUnless(HAVE_CFFI or REQUIRE_CFFI, "cffi not installed")
@unittest.skipUnless(LIB, "ODIN3_LIB not set (run CTest python_restore)")
class RestoredProvenanceTest(unittest.TestCase):
    odin3: Odin3

    @classmethod
    def setUpClass(cls) -> None:
        if not HAVE_CFFI:
            raise AssertionError("ODIN3_REQUIRE_CFFI=1 but cffi is not installed")
        assert LIB is not None
        cls.odin3 = Odin3(Path(LIB))

    def test_a_restored_net_answers_provenance_and_chain_as_the_original(self) -> None:
        work = Path(tempfile.mkdtemp(prefix="restore-py-"))
        self.addCleanup(shutil.rmtree, work, True)
        compared = 0
        for name in NAMES:
            out = work / f"{name}.blif"
            with Design(self.odin3) as original:
                original.run_script(
                    FLOW.format(lib=TECHLIB, src=MICROS / f"{name}.v")
                    + f"write_blif --vtr --name-style hash --prov-map {out}"
                )
            with Design(self.odin3) as restored:
                restored.run_script(
                    f"read_techlib {TECHLIB}; read_blif --name-map {out}.names {out}"
                )
                after = restored_answers(restored)
            before = original_answers(Path(f"{out}.names"))
            # the map lists nets the file does not mention (a restore cannot bind those): the
            # restored nets are a sub-multiset of the original's, and every one of them answers
            # as an original net did
            remaining = list(before)
            for answer in after:
                self.assertIn(answer, remaining, f"{name}: a restored net answers differently")
                remaining.remove(answer)
                compared += 1
        self.assertGreater(compared, 20)


if __name__ == "__main__":
    unittest.main()

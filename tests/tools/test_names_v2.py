"""V1.2.0 Task 6: the `names` pass on version 2 maps (--lookup, --chain, --check).

Maps are written by the Task 5 writers (`write_blif --vtr`, `write_json`, `write_verilog` with
`--name-style hash --prov-map`) from two micro designs run through the VTR flow to `lower` (no
ABC, so the test runs in every build). The golden text under tests/tools/data/names_v2 holds the
`--lookup` and `--chain` output for a fixed choice of objects (each line ends with `$`);
ODIN3_UPDATE_GOLDEN=1 rewrites it.
Version 1 goldens (`v1_*.txt`) were produced by the `odin3` of main before this task and pin that
`names` output on a version 1 map is unchanged. Needs ODIN3_CLI (CTest python_tools sets it).
Work files go to a fresh temporary directory that is left in place.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

from .helpers import REPO_ROOT

ODIN3 = os.environ.get("ODIN3_CLI")
GOLDEN = Path(__file__).resolve().parent / "data" / "names_v2"
UPDATE = os.environ.get("ODIN3_UPDATE_GOLDEN") == "1"
MICRO = "tests/micro/verilog/full"
FLOW = (
    "read_techlib lib/vtr.o3lib; read_verilog --nolatches --vtr-compat {ast} {src}; proc; opt; "
    "flatten; opt -full; wreduce; hardmap; lower; "
)
WRITERS = {
    "blif": "write_blif --vtr --name-style hash {map} {out}",
    "json": "write_json --name-style hash {map} {out}",
    "v": "write_verilog --name-style hash {map} {out}",
}
CASES = [("ansiportlist_2", "blif"), ("ch_intrinsics", "blif"), ("ch_intrinsics", "json"),
         ("ch_intrinsics", "v")]


def names(args: str) -> tuple[int, str, str]:
    """Run the names pass through the CLI; (exit code, stdout, stderr)."""
    assert ODIN3 is not None
    run = subprocess.run([ODIN3, "-p", f"names {args}"], cwd=REPO_ROOT, capture_output=True,
                         text=True, check=False)
    return run.returncode, run.stdout, run.stderr


def write_map(work: Path, micro: str, writer: str, version: int, folder: str = MICRO) -> Path:
    """The name map of one micro through one writer; version 1 is written without --prov-map."""
    assert ODIN3 is not None
    out = work / f"{micro}{'' if version == 2 else '_v1'}.{writer}"
    script = FLOW.format(ast="--prov-ast refs" if version == 2 else "", src=f"{folder}/{micro}.v")
    script += WRITERS[writer].format(map="--prov-map" if version == 2 else "", out=out)
    run = subprocess.run([ODIN3, "-p", script], cwd=REPO_ROOT, capture_output=True, text=True,
                         check=False)
    if run.returncode != 0:
        raise AssertionError(run.stderr)
    return Path(str(out) + ".names")


def marked(out: str) -> str:
    """out with a `$` after every line, so that a trailing TAB (an empty last field) survives the
    pre-commit whitespace fixer in the golden files."""
    return "".join(f"{line}$\n" for line in out.splitlines())


def pick(lines: list[str]) -> list[str]:
    """The objects of the golden: the first O, the object named by the first join record, by the
    first context record, the first N and the last O (their names for --lookup/--chain)."""
    rec_of = {}
    for line in lines:
        if line.startswith("R\t"):
            fields = line.split("\t")
            rec_of[fields[1]] = fields
    def named_by(test: object) -> str | None:
        for line in lines:
            if line[:2] in ("O\t", "N\t"):
                fields = line.split("\t")
                rec = fields[8] if fields[0] == "O" else fields[4]
                if rec in rec_of and test(rec_of[rec]):  # type: ignore[operator]
                    return fields[3]
        return None
    objects = [x for x in lines if x[:2] in ("O\t", "N\t")]
    chosen = [objects[0].split("\t")[3],
              named_by(lambda r: "join" in r[9].split(",")),
              named_by(lambda r: r[4].startswith("context:")),
              next(x for x in objects if x[0] == "N").split("\t")[3],
              next(x for x in reversed(objects) if x[0] == "O").split("\t")[3]]
    seen: list[str] = []
    for name in chosen:
        if name is not None and name not in seen:
            seen.append(name)
    return seen


@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools sets it)")
class NamesV2Test(unittest.TestCase):
    work: Path
    maps: dict[tuple[str, str], Path]

    @classmethod
    def setUpClass(cls) -> None:
        base = Path("/tmp/claude-1000")
        cls.work = Path(tempfile.mkdtemp(prefix="names-v2-", dir=base if base.is_dir() else None))
        cls.maps = {case: write_map(cls.work, case[0], case[1], 2) for case in CASES}

    def golden(self, name: str, text: str) -> None:
        path = GOLDEN / name
        if UPDATE:
            path.write_text(text)
        self.assertEqual(path.read_text(), text, f"{name} differs (ODIN3_UPDATE_GOLDEN=1 rewrites)")

    def test_lookup_and_chain_goldens(self) -> None:
        for (micro, writer), path in self.maps.items():
            lines = path.read_text().split("\n")
            text = ""
            for name in pick(lines):
                for action in ("lookup", "chain"):
                    code, out, err = names(f"--map {path} --{action} {name}")
                    self.assertEqual(code, 0, err)
                    text += f"== --{action} {name}\n" + marked(out)
            self.golden(f"{micro}.{writer}.txt", text)

    def test_every_line_type_is_printed(self) -> None:
        seen: set[str] = set()
        for path in self.maps.values():
            for name in pick(path.read_text().split("\n")):
                _code, out, _err = names(f"--map {path} --lookup {name}")
                seen.update(line.lstrip()[0] for line in out.splitlines())
        self.assertEqual(seen, set("ONRCFT"))

    def test_check_counts_and_accepts_every_map(self) -> None:
        for path in self.maps.values():
            code, out, err = names(f"--map {path} --check")
            self.assertEqual(code, 0, err)
            first, second = out.splitlines()
            self.assertIn("bad_hash=0 duplicates=0 ignored=0", first)
            self.assertTrue(second.startswith("records="), second)
            self.assertIn("bad_id=0 bad_ref=0 bad_order=0 bad_class=0 no_record=0", second)

    def test_lookup_of_an_absent_name_fails(self) -> None:
        path = self.maps[("ansiportlist_2", "blif")]
        for action in ("lookup", "chain"):
            code, out, err = names(f"--map {path} --{action} h0000000000")
            self.assertNotEqual(code, 0)
            self.assertEqual(out, "")
            self.assertIn("is not in", err)

    def test_n_line_matches_by_name(self) -> None:
        path = self.maps[("ansiportlist_2", "blif")]
        code, out, _err = names(f"--map {path} --lookup in_1[0]")
        self.assertEqual(code, 0)
        self.assertTrue(out.startswith("N\tansiportlist\tnet\tin_1[0]\t"), out)

    def test_a_line_is_parsed_and_changes_nothing(self) -> None:
        path = self.maps[("ansiportlist_2", "blif")]
        text = path.read_text()
        extra = self.work / "with_a.names"
        extra.write_text(text + "A\tatom_0\tr1\t-\n")
        self.assertEqual(names(f"--map {extra} --check")[1], names(f"--map {path} --check")[1])
        name = pick(text.split("\n"))[0]
        self.assertEqual(names(f"--map {extra} --lookup {name}")[1],
                         names(f"--map {path} --lookup {name}")[1])
        short = self.work / "short_a.names"
        short.write_text(text + "A\tatom_0\n")  # too few fields: ignored with a warning
        self.assertNotEqual(names(f"--map {short} --check")[0], 0)

    def test_chain_needs_a_version_2_map(self) -> None:
        old = write_map(self.work, "ansiportlist_2", "blif", 1)
        code, _out, err = names(f"--map {old} --chain h0000000000")
        self.assertNotEqual(code, 0)
        self.assertIn("needs a version 2 map", err)

    def test_version_1_output_is_unchanged(self) -> None:
        for micro in ("ansiportlist_2", "ch_intrinsics"):
            path = write_map(self.work, micro, "blif", 1)
            lines = path.read_text().split("\n")
            short = lines[1].split("\t")[2]
            full = lines[1].split("\t")[3]
            text = ""
            for args in ("--check", f"--lookup {short}", f"--reverse {full}"):
                code, out, err = names(f"--map {path} {args}")
                self.assertEqual(code, 0, err)
                text += f"== {args.split()[0]}\n{out}"
            self.golden(f"v1_{micro}.txt", text)


def edit(text: str, prefix: str, change: object, nth: int = 0) -> str:
    """text with the nth line that starts with prefix replaced by change(fields)."""
    lines = text.split("\n")
    hits = [i for i, line in enumerate(lines) if line.startswith(prefix)]
    fields = lines[hits[nth]].split("\t")
    lines[hits[nth]] = "\t".join(change(fields))  # type: ignore[operator]
    return "\n".join(lines)


def with_field(index: int, value: str) -> object:
    def change(fields: list[str]) -> list[str]:
        fields[index] = value
        return fields
    return change


@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools sets it)")
class NamesV2CheckTest(unittest.TestCase):
    """--check rejects each malformed case of the Task 5 unit tests (test_name_map.c)."""

    text: str
    work: Path

    @classmethod
    def setUpClass(cls) -> None:
        base = Path("/tmp/claude-1000")
        cls.work = Path(
            tempfile.mkdtemp(prefix="names-v2-bad-", dir=base if base.is_dir() else None))
        cls.text = write_map(cls.work, "ch_intrinsics", "blif", 2).read_text()

    def rejected(self, label: str, text: str, want: str) -> None:
        path = self.work / f"{label}.names"
        path.write_text(text)
        code, out, err = names(f"--map {path} --check")
        self.assertNotEqual(code, 0, label)
        self.assertIn(want, err + out, label)

    def test_the_unmodified_map_passes(self) -> None:
        path = self.work / "good.names"
        path.write_text(self.text)
        self.assertEqual(names(f"--map {path} --check")[0], 0)

    def test_planted_problems(self) -> None:
        text = self.text
        join = "R\tr45\t"
        cases = {
            "bad_id": (edit(text, "R\tr2\t", with_field(1, "r99999")), "id is not"),
            "parent_unresolved": (edit(text, join, with_field(6, "r99999")), "do not resolve"),
            "parent_after_child": (edit(text, join, with_field(6, "r46")), "not numbered before"),
            "self_parent": (edit(text, join, with_field(6, "r45")), "not numbered before"),
            "bad_dir": (edit(text, join, with_field(4, "sideways:word")), "no valid kind"),
            "bad_step": (edit(text, join, with_field(5, "t99.1")), "no valid kind"),
            "bad_file": (edit(text, "R\tr1\t", with_field(7, "f9:1.1-1.2")), "do not resolve"),
            "bad_cons": (edit(text, join, with_field(8, "c99999")), "do not resolve"),
            "no_record": (edit(text, "O\t", with_field(8, "r99999")), "record is missing"),
            "construct_parent_after": (edit(text, "C\tc2\t", with_field(4, "c3")),
                                       "not numbered before"),
            "too_few_fields": (edit(text, "R\tr3\t", lambda f: f[:3]), "ignored"),
            "bad_hash": (edit(text, "O\t", with_field(3, "hxxxxxxxxxx")), "ignored"),
            "unknown_type": (text + "Z\tx\n", "ignored"),
            "bad_escape": (edit(text, "O\t", with_field(1, "m%2G")), "ignored"),
            "carriage_return": (edit(text, "R\tr3\t", lambda f: f[:-1] + [f[-1] + "\r"]),
                                "ignored"),
            "unknown_version": (text.replace("odin3-names 2", "odin3-names 3", 1), "version"),
            "bad_header": (text.replace(" prov=1", "", 1), "header"),
            "not_a_map": ("hello\n" + text, "not a name map"),
        }
        for label, (changed, want) in cases.items():
            with self.subTest(label):
                self.rejected(label, changed, want)

    def test_duplicate_object_is_counted(self) -> None:
        lines = self.text.split("\n")
        first_o = next(line for line in lines if line.startswith("O\t"))
        twice = self.text.replace(first_o + "\n", first_o + "\n" + first_o + "\n", 1)
        self.rejected("duplicate", twice, "duplicates")


if __name__ == "__main__":
    unittest.main()

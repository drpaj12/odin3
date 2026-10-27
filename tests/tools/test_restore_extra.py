"""V1.2.0 Task 9 (spec 4.5): the post-Odin lines (A, Q, K), the stale marks, ast=none, version 1
maps and the prov_post extension, through the odin3 CLI (ODIN3_CLI).

  - a map with A, Q and K lines is restored and written again with the same lines (verbatim, with
    the numbers of the new map), and write 2 equals write 3;
  - restore, then `opt`, then write: the restored A, Q and K lines carry `stale=t<n>` (the T row
    of opt), the post-Odin records only they reach carry `stale=t<n>` in their flags, a new join of
    `opt` has a Q line with stale `-` and the objective `opt`; a further restore keeps all of it;
  - ast=none: restore, write again, header and no C lines;
  - version 1 map: no restore, the files a v1 round trip gives are unchanged;
  - odin3-prov-post's extension of a flow directory restores, and the lines of the rewrite equal
    the lines of the extended map, every A, Q, K line and every record kept (every record
    odin3-prov-post makes has a parent, so none is dropped).
Work files are made under TMPDIR and left in place.
"""

from __future__ import annotations

import shutil
import unittest
from pathlib import Path

import prov_post

from .helpers import FIXTURES
from .restore_common import LIB, MICRO_DIR, ODIN3, flow, run_cli, workdir

BLIF_READ = "read_techlib {lib}; read_blif --name-map {names} {src}; "
JSON_READ = "read_techlib {lib}; read_json --name-map {names} {src}; "


def lines(path: Path) -> list[str]:
    return path.read_text().splitlines()


def names_of(path: Path) -> Path:
    return Path(f"{path}.names")


def write_first(
    micro: str,
    kind: str,
    out: Path,
    ast: str = "--prov-ast refs",
    prov: str = "--prov-map",
    steps: str = "",
) -> None:
    """The VTR flow (with `steps` instead of its optimizing passes when given) and a hash-named,
    version 2 write."""
    script = flow(micro, ast)
    if steps:
        script = (
            f"read_techlib {LIB}; read_verilog --nolatches --vtr-compat {ast} "
            f"{MICRO_DIR / (micro + '.v')}; {steps}"
        )
    writer = "write_blif --vtr" if kind == "blif" else "write_json"
    run = run_cli(f"{script}{writer} --name-style hash {prov} {out}")
    assert run.returncode == 0, run.stderr


def rewrite(kind: str, names: Path, src: Path, out: Path, middle: str = "") -> str:
    """Restore src with names, run `middle`, write again; the stderr (the exit code is checked)."""
    template = BLIF_READ if kind == "blif" else JSON_READ
    writer = "write_blif --vtr" if kind == "blif" else "write_json"
    script = template.format(lib=LIB, names=names, src=src) + middle
    run = run_cli(f"{script}{writer} --name-style hash --prov-map {out}")
    assert run.returncode == 0, run.stderr
    return run.stderr


def last_field(line: str) -> str:
    return line.split("\t")[-1]


@unittest.skipUnless(ODIN3, "ODIN3_CLI is not set")
class RestoreExtra(unittest.TestCase):
    def test_a_q_k_lines_round_trip_verbatim_and_write_2_equals_write_3(self) -> None:
        work = workdir("extra")
        first = work / "mux.blif"
        write_first("bm_DL_2_1_mux", "blif", first)
        extras = [
            "A\tatom1\tr1,r2\t-",
            "Q\tr2\t\topt\t\t\t\t\t\t\t\tbasis\t-",
            "K\tr2\tm\t\t\t\tbasis\t-",
        ]
        with names_of(first).open("a") as handle:
            handle.write("\n".join(extras) + "\n")
        second, third = work / "mux.2.blif", work / "mux.3.blif"
        rewrite("blif", names_of(first), first, second)
        rewrite("blif", names_of(second), second, third)
        self.assertEqual(set(lines(names_of(first))), set(lines(names_of(second))))
        self.assertEqual(names_of(second).read_bytes(), names_of(third).read_bytes())
        self.assertEqual(
            extras, [x for x in lines(names_of(second)) if x[:2] in ("A\t", "Q\t", "K\t")]
        )

    def test_restore_opt_write_marks_stale_and_a_new_join_has_a_fresh_q(self) -> None:
        work = workdir("stale")
        first = work / "cmp.json"
        write_first(
            "bm_stmt_compare_padding", "json", first, steps="proc; flatten; hardmap; lower; "
        )
        nm = lines(names_of(first))
        n_recs = sum(1 for x in nm if x.startswith("R\t"))
        n_runs = sum(1 for x in nm if x.startswith("T\t"))
        pack = n_runs + 1
        extra = [
            f"T\tt{pack}\tpack\t\t",
            f"R\tr{n_recs + 1}\tD\tble\tcoarsen:lut\tt{pack}.1\tr1,r2\t\t\tx=1",
            f"R\tr{n_recs + 2}\tD\tclb\tcoarsen:ble\tt{pack}.2\tr{n_recs + 1}\t\t\t",
            f"A\tatom1\tr{n_recs + 1},r{n_recs + 2}\t-",
            "Q\tr3\t\tpack\t\t\t\t\t\t\t\tbasis\t-",
            "K\tr3\tm\t\t\t\tbasis\t-",
        ]
        with names_of(first).open("a") as handle:
            handle.write("\n".join(extra) + "\n")
        plain, after = work / "cmp.2.json", work / "cmp.3.json"
        rewrite("json", names_of(first), first, plain)
        post_lines = ("A\t", "Q\t", "K\t")
        self.assertEqual(
            [x for x in extra if x[:2] in post_lines],
            [x for x in lines(names_of(plain)) if x[:2] in post_lines],
        )
        # an unchanged restore keeps the lines non-stale
        self.assertTrue(
            all(
                last_field(x) == "-"
                for x in lines(names_of(plain))
                if x[:2] in ("A\t", "Q\t", "K\t")
            )
        )
        rewrite("json", names_of(plain), plain, after, middle="opt -full; ")
        out = lines(names_of(after))
        opt_row = next(
            x.split("\t")[1] for x in out if x.startswith("T\t") and x.split("\t")[2] == "opt"
        )
        old = [x for x in out if x[:2] in ("A\t", "Q\t", "K\t") and last_field(x) != "-"]
        self.assertEqual(3, len(old), "A, Q and K are stale")
        self.assertTrue(all(last_field(x) == opt_row for x in old))
        post = [x for x in out if x.startswith("R\t") and "stale=" in x]
        self.assertEqual(2, len(post), "the ble and the clb are stale")
        self.assertTrue(all(x.endswith(f"stale={opt_row}") for x in post))
        self.assertTrue(any("x=1,stale=" in x for x in post), "the foreign flag is kept")
        fresh = [x for x in out if x.startswith("Q\t") and last_field(x) == "-"]
        self.assertTrue(fresh, "opt made joins")
        self.assertTrue(all(x.split("\t")[3] == "opt" for x in fresh))
        self.assertTrue(all(x.split("\t")[1] not in {y.split("\t")[1] for y in old} for x in fresh))
        # a further restore keeps all of it, and write 3 equals write 4
        again, again2 = work / "cmp.4.json", work / "cmp.5.json"
        rewrite("json", names_of(after), after, again)
        rewrite("json", names_of(again), again, again2)
        self.assertEqual(names_of(again).read_bytes(), names_of(again2).read_bytes())
        kept = [x for x in lines(names_of(again)) if x[:2] in ("A\t", "Q\t", "K\t")]
        self.assertEqual(len([x for x in out if x[:2] in post_lines]), len(kept))
        self.assertEqual(
            2, sum(1 for x in lines(names_of(again)) if x.startswith("R\t") and "stale=" in x)
        )

    def test_task10_enriched_q_survives_an_unchanged_restore_and_goes_stale_after_opt(self) -> None:
        """Spec 3.6 and 4.5: a restored Q wins over a new objective-only one only while it is not
        stale. An unchanged restore then write keeps the enriched line; restore, opt, write gives
        the restored Q (and its K) with the stale run of opt, plus a fresh objective-only Q with
        stale `-` for each join opt made."""
        work = workdir("task10")
        first = work / "cmp.json"
        write_first(
            "bm_stmt_compare_padding", "json", first, steps="proc; flatten; hardmap; lower; "
        )
        self.assertFalse([x for x in lines(names_of(first)) if x.startswith("Q\t")])
        basis = "why=r,objective=i,gain=r,pcrit=r,rho=r,slack=r,near=r,npaths=r"
        enriched = [
            f"Q\tr3\tSC:0.500,FL:0.500\tnorns-pack\tseed:1.000\t0.900\t1.000\t-1.000\t5:1,10:1\t"
            f"5:2,10:3\t2\t{basis}\t-",
            "K\tr3\tm\t0.900\t1.000\t-1.000\tpcrit=r,rho=r,slack=r\t-",
        ]
        with names_of(first).open("a") as handle:
            handle.write("\n".join(enriched) + "\n")
        same, after = work / "cmp.2.json", work / "cmp.3.json"
        rewrite("json", names_of(first), first, same)
        post = ("Q\t", "K\t")
        self.assertEqual(enriched, [x for x in lines(names_of(same)) if x.startswith(post)])
        rewrite("json", names_of(same), same, after, middle="opt -full; ")
        out = [x for x in lines(names_of(after)) if x.startswith(post)]
        opt_row = next(
            x.split("\t")[1]
            for x in lines(names_of(after))
            if x.startswith("T\t") and x.split("\t")[2] == "opt"
        )
        stale = [x for x in out if last_field(x) == opt_row]
        self.assertEqual([x.rsplit("\t", 1)[0] for x in stale],
                         [x.rsplit("\t", 1)[0] for x in enriched])
        fresh = [x for x in out if x.startswith("Q\t") and last_field(x) == "-"]
        self.assertTrue(fresh, "opt made joins")
        for line in fresh:
            fields = line.split("\t")
            self.assertEqual(fields[3], "opt")
            self.assertTrue(all(f == "" for f in [fields[2], *fields[4:11]]))
            self.assertIn("objective=r", fields[11])
            self.assertNotEqual(fields[1], "r3")

    def test_a_pass_that_only_looks_leaves_the_lines_current(self) -> None:
        work = workdir("look")
        first = work / "mux.blif"
        write_first("bm_DL_2_1_mux", "blif", first)
        with names_of(first).open("a") as handle:
            handle.write("A\tatom1\tr1,r2\t-\nQ\tr2\t\topt\t\t\t\t\t\t\t\tbasis\t-\n")
        looked, mutated = work / "mux.2.blif", work / "mux.3.blif"
        rewrite("blif", names_of(first), first, looked, middle="stats; check; ")
        text = names_of(looked).read_text()
        self.assertNotIn("stale=", text)
        self.assertTrue(
            all(last_field(x) == "-" for x in lines(names_of(looked)) if x[:2] in ("A\t", "Q\t"))
        )
        rewrite("blif", names_of(first), first, mutated, middle="opt; ")
        self.assertTrue(
            all(last_field(x) != "-" for x in lines(names_of(mutated)) if x[:2] in ("A\t", "Q\t"))
        )

    def test_ast_none_restores_and_writes_no_construct(self) -> None:
        work = workdir("none")
        first = work / "enc.blif"
        write_first("bm_DL_2_4_encoder", "blif", first, ast="")
        self.assertIn("ast=none", lines(names_of(first))[0])
        second = work / "enc.2.blif"
        rewrite("blif", names_of(first), first, second)
        text = lines(names_of(second))
        self.assertIn("ast=none", text[0])
        self.assertFalse([x for x in text if x.startswith("C\t")])
        self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_a_version_1_map_restores_nothing_and_changes_nothing(self) -> None:
        work = workdir("v1")
        first = work / "mux.blif"
        script = flow("bm_DL_2_1_mux", "") + f"write_blif --vtr --name-style hash {first}"
        self.assertEqual(0, run_cli(script).returncode)
        self.assertTrue(lines(names_of(first))[0].startswith("# odin3-names 1 "))
        second = work / "mux.2.blif"
        run = run_cli(
            BLIF_READ.format(lib=LIB, names=names_of(first), src=first)
            + f"write_blif --vtr --name-style hash {second}"
        )
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertNotIn("name map restore", run.stderr)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(names_of(first).read_bytes(), names_of(second).read_bytes())

    def v1_map_changes_nothing(self, kind: str, micro: str, reader: str, suffix: str) -> None:
        """A version 1 map: the reader restores nothing, and the second write equals the first
        (netlist and map), for a micro the pair proved idempotent (step 0)."""
        work = workdir(f"v1{kind}")
        first = work / f"{micro}.{suffix}"
        writer = "write_json" if kind == "json" else "write_verilog"
        script = flow(micro, "") + f"{writer} --name-style hash {first}"
        self.assertEqual(0, run_cli(script).returncode)
        self.assertTrue(lines(names_of(first))[0].startswith("# odin3-names 1 "))
        second = work / f"{micro}.2.{suffix}"
        run = run_cli(
            f"read_techlib {LIB}; {reader.format(names=names_of(first), src=first)}"
            f"{writer} --name-style hash {second}"
        )
        self.assertEqual(0, run.returncode, run.stderr)
        self.assertNotIn("name map restore", run.stderr)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(names_of(first).read_bytes(), names_of(second).read_bytes())

    def test_a_version_1_map_changes_nothing_for_the_json_reader(self) -> None:
        self.v1_map_changes_nothing(
            "json", "bm_lpm_concat", "read_json --name-map {names} {src}; ", "json"
        )

    def test_a_version_1_map_changes_nothing_for_the_verilog_reader(self) -> None:
        self.v1_map_changes_nothing(
            "v", "bm_DL_2_1_mux", "read_verilog -netlist -name-map {names} {src}; proc; ", "v"
        )

    def test_prov_post_extension_restores_and_rewrites_the_same_lines(self) -> None:
        work = workdir("post")
        for design in ("bm_DL_logic_w_Dff", "bm_DL_BCD_adder"):
            flow_dir = work / design
            shutil.copytree(FIXTURES / "prov_post" / design, flow_dir)
            self.assertEqual(0, prov_post.main([str(flow_dir)]))
            blif = flow_dir / "odin3" / f"{design}.blif"
            second, third = work / f"{design}.2.blif", work / f"{design}.3.blif"
            err = rewrite("blif", names_of(blif), blif, second)
            rewrite("blif", names_of(second), second, third)
            self.assertEqual(names_of(second).read_bytes(), names_of(third).read_bytes())
            before, after = set(lines(names_of(blif))), set(lines(names_of(second)))
            self.assertNotIn("not restored", err)
            self.assertNotIn("no parent", err)
            if design == "bm_DL_logic_w_Dff":
                self.assertEqual(before, after, "the set of lines of the prov_post file")
            else:
                # a port vector keeps one record (its first bit's): records are renumbered and
                # BLEs over the bits of one port become one record (hash-consing); every A, Q, K
                # line is kept, and no record is dropped for lack of a parent
                self.assertEqual(
                    sorted(x.split("\t")[1] for x in before if x.startswith("A\t")),
                    sorted(x.split("\t")[1] for x in after if x.startswith("A\t")),
                )
                # Q and K lines are keyed by record number, which the renumbering changes
                for letter in ("Q\t", "K\t"):
                    self.assertEqual(
                        sum(1 for x in before if x.startswith(letter)),
                        sum(1 for x in after if x.startswith(letter)),
                    )
                for level in ("ble", "clb", "site"):
                    count_after = sum(
                        1 for x in after if x.startswith("R\t") and f"\t{level}\t" in x
                    )
                    count_before = sum(
                        1 for x in before if x.startswith("R\t") and f"\t{level}\t" in x
                    )
                    self.assertTrue(0 < count_after <= count_before, level)
            self.assertTrue(any(x.startswith("R\t") and "\tsite\t" in x for x in after))
            check = run_cli(f"names --map {names_of(second)} --check")
            self.assertEqual(0, check.returncode, check.stderr)


if __name__ == "__main__":
    unittest.main()

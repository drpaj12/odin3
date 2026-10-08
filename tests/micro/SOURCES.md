# tests/micro sources

Copied verbatim (byte-identical, `-text` in `.gitattributes`) on 2026-10-08 from VTR
`odin_ii/regression_test/benchmark/verilog/` at commit `3c9a4d23b27d187bd67c79dc01a18ff1ffc21d9b`
(the commit in `docs/ORACLES.md`). The layout under `verilog/` mirrors upstream, so a micro's
path here matches its golden name in `odin3-golden` (`<arch>/regression/verilog/<path>`).

- 539 `.v` and 71 `.vh` files: every `.v`/`.vh` under that directory except the exclusions below.
- Test vectors, `task/` configs and `suite/` lists were not copied.

## Excluded (goldens still exist in odin3-golden)

| Upstream path | Why |
|---|---|
| `verilog/koios_dummy/*.v` (23 files) | Wrappers that `include` full Koios designs from `vtr_flow/benchmarks/verilog/koios/` outside this tree. They are not micros. |
| `verilog/full/mcml.v` | 674 KB, over the 500 KB `check-added-large-files` limit. A full design that differs from VTR-19's `mcml.v` from line 492 on. |

## Known upstream breakage, copied as-is

- `verilog/large/arm_core.v` includes `a25/*.v` files that do not exist upstream.

## Refresh

When `external/` moves to a new VTR commit, re-copy with the same filter and update the commit above:

```sh
B=../external/vtr-verilog-to-routing/odin_ii/regression_test/benchmark
(cd $B && find verilog \( -name '*.v' -o -name '*.vh' \) ! -path 'verilog/koios_dummy/*' \
    ! -path 'verilog/full/mcml.v' -print0 | sort -z | tar --null -cf - -T -) | tar -xf - -C tests/micro
```

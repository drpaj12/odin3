# `vtr-qor table` golden fixture (3H plan Task 9; spec §6, T3, T7)

`runs/` is a synthetic job tree, laid out as `--out` is:
`3c9a4d23b27d/k6_frac_N10_frac_chain_mem32K_40nm/<leg>/<key12>/<design>/{status,metrics.json,job.json}`,
with each leg's `latest` file. It holds four legs, each on the 15 gate designs, the 2 extras and the
4 offload designs (84 jobs, all `ok`, VTR at the pin; the table's default sets leave the offload
designs out, so a table holds 68 of them):

| leg | key12 |
|---|---|
| P | `111111111111` |
| O′ (`Oprime`) | `222222222222` |
| P′ (`Pprime`) | `333333333333` |
| O | `444444444444` |

`vtr/` is a fake VTR tree. It holds only what the cross-check reads:
- `vtr_reg_qor_chain`'s `config.txt` and `golden_results.txt`;
- the pass requirements file the config names, with a `%include`.

`expected/` holds what `vtr-qor table --legs P,O',P',O --label fixture` writes there:
`tables/20261013-0441-fixture/{qor.md,runs.csv,ratios.csv,runs.json}`. The stamp is the newest
`job.json` end time, O `arm_core` at `2026-10-13T04:41:00Z`. Its host, `Newest CPU @ 3.0GHz,
31.2 GiB`, is the only job with that host. `test_table_golden_files` compares the files byte for
byte.

## The values

**P on every design:**
- `num_clb` 200, `min_chan_width` 50, CPD 2.5 ns (2.4 on `sha`), wirelength 1000;
- `luts` 1000, `ffs` 100, `adders` 10, `mults` 2, `depth` = `abc_depth` 5;
- `ram_slices` 4 and `num_memories` 1, except 0 on the 9 RAM-free gate designs;
- `num_mult` 2, device 20×20;
- front end 10.0 s / 100 MiB, VPR 20.0 s / 300 MiB;
- fcheck `pass`.

The `mcml` P job ran in `odin3-heavyxl.slice` (`xl`).

**O on gate design i (0-based, in §2 order):**
- `num_clb` 275 for even i < 18, 176 for odd i, and 220 for i = 18 (`LU32PEEng`, an offload
  design: not in the table);
- `min_chan_width` 60;
- CPD 2.5, except 3.6 on `sha`;
- wirelength 1050;
- `luts` 1100, `depth` = `abc_depth` 4, `adders` 0 on `ch_intrinsics` (else 10);
- front end 5.0 s / 200 MiB, VPR 22.0 s / 330 MiB;
- fcheck `pass`, equiv `equivalent` (RAM-free) or `ram`, rerun `same`, exchange `aig` (`blif` on `mcml`).

The O `sha` job has a `cost_warnings` line (`*`).

**O on the extras:** `num_clb` 260, W 65, CPD 3.25, wirelength 1300.

**O′:** as P, except `num_clb` 220, W 55, wirelength 1050, `luts` 1050, and front end 6.0 s / 150 MiB.

**P′:** as P, except:
- `num_clb` 180, wirelength 950, `luts` 950;
- front end 8.0 s / 120 MiB;
- exchange `aig`;
- `depth` -1 on `bgm` (a loop, so missing).

**VTR's golden** equals P everywhere except:
- `num_clb` 160 on `ch_intrinsics`;
- W 40 on `diffeq1`;
- CPD -1 (missing) on `stereovision3`.

The bands are `num_clb` 0.90–1.10, W 0.25–1.30, CPD 0.50–1.40 and wirelength 0.60–1.50.

## Hand-computed geomeans (over the 15 unless noted)

### O/P (the gate)

| metric | per-design ratios | geomean | printed |
|---|---|---|---|
| `num_clb` | 8 × 1.375 (275/200), 7 × 0.88 (176/200); (1.375^8 × 0.88^7)^(1/15) | 1.11649 | `1.116`; max `1.375 (ch_intrinsics)` |
| `min_chan_width` | 60/50 = 1.2 on all 15 | 1.2 | `1.2000`: within 0.005 of the 1.20 bound, so 4 decimals; it passes (inclusive) |
| CPD | 1.0 × 14, `sha` 3.6/2.4 = 1.5 | 1.5^(1/15) = e^(0.405465/15) = 1.027399 | `1.027`; `sha` `1.5000` (on the bound, passes) |
| wirelength | 1050/1000 = 1.05 | 1.05 | `1.050` |

The verdict is PASS, with no violations. On the extras, every gated ratio is 1.3, so their
geomean (2) is `1.300`.

### Attribution

| metric | O′/P | P′/P | O/O′ | O/P |
|---|---|---|---|---|
| `num_clb` | 1.100 | 0.900 | 8 × 1.25, 7 × 0.8 → (1.25)^(1/15) = **1.015** | 1.100 |
| `min_chan_width` | 1.100 | 1.000 | 60/55 = **1.091** | 1.2000 |
| CPD | 1.000 | 1.000 | 1.5^(1/15) = **1.027** | 1.027 |
| wirelength | 1.050 | 0.950 | 1.000 | 1.050 |

### Report-only ratios (`ratios.csv`)

| pair | metric | ratios | geomean | printed |
|---|---|---|---|---|
| O/P | `adders` | `ch_intrinsics` 0/10 = 0.0 | 0.0 (a zero factor makes the geomean 0) | `0.000` |
| O/P | `ram_slices` | 0/0 = 1.0 on RAM-free designs | | `1.000` |
| O/P | `depth` | 4/5 | 0.8 | `0.800` |
| O/P | front-end peak | 200/100 | 2.0 | `2.000` |
| P′/P | `depth` | none: `bgm`, where P′ depth is -1, is an offload design and not in the table | 1.0 | `1.000`; `runs.json` `n` 15 of 15 |
| O′/P | front-end peak | 150/100 = 1.5 | 1.5 | `1.5000`: near a bound, so 4 decimals |
| O/O′ | `num_clb` on the extras | 260/220 | 1.18182 | `1.182` |

### Cross-check, P/golden

| metric | ratios | geomean | printed | `outside` |
|---|---|---|---|---|
| `num_clb` | 14 × 1.0, `ch_intrinsics` 200/160 = 1.25 | 1.25^(1/15) = e^(0.223144/15) = 1.014987 | `1.015` | `ch_intrinsics` (1.25 > 1.10) |
| W | `diffeq1` 50/40 = 1.25 | 1.014987 | `1.015` | none: 1.25 ≤ 1.30 |
| CPD | `stereovision3` missing | 14 × 1.0 | `1.000 (14/15)` | |
| wirelength | | 1.0 | `1.000` | |

## Regenerating

If a writer changes on purpose, rerun the table over a copy of `runs/` and replace `expected/`
after reading the diff:

```
cp -r runs/. /tmp/claude-1000/t/
tools/vtr-qor/vtr-qor table --out /tmp/claude-1000/t --vtr-root tests/tools/fixtures/vtr_qor/table/vtr --legs "P,O',P',O" --label fixture
```

The geomeans above must not move.

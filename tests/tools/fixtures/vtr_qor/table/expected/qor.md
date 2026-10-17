# Odin III QoR — k6_frac_N10_frac_chain_mem32K_40nm — fixture

VTR 3c9a4d23b27d · odin3 0123456789ab (build bbbbbbbbbbbb) · ABC script 555555555555 · legs P 111111111111, O 444444444444, O' 222222222222, P' 333333333333
· host Newest CPU @ 3.0GHz, 31.2 GiB · 2026-10-13T04:41:00Z

## Gate (decision 3): PASS

| design | CLB P | CLB O | O/P | W P | W O | O/P | CPD P | CPD O | O/P | WL P | WL O | O/P | ok |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ch_intrinsics | 200 | 275 | 1.375 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| stereovision3 | 200 | 176 | 0.880 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| diffeq2 | 200 | 275 | 1.375 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| diffeq1 | 200 | 176 | 0.880 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| mkPktMerge | 200 | 275 | 1.375 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| raygentop | 200 | 176 | 0.880 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| sha | 200 | 275 | 1.375 | 50 | 60 | 1.2000 | 2.400 | 3.600 | 1.5000 | 1000 | 1050 | 1.050 | ✓ |
| boundtop | 200 | 176 | 0.880 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| mkSMAdapter4B | 200 | 275 | 1.375 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| or1200 | 200 | 176 | 0.880 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| stereovision0 | 200 | 275 | 1.375 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| stereovision1 | 200 | 176 | 0.880 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| mkDelayWorker32B | 200 | 275 | 1.375 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| stereovision2 | 200 | 176 | 0.880 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| blob_merge | 200 | 275 | 1.375 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| LU8PEEng | 200 | 176 | 0.880 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| bgm | 200 | 275 | 1.375 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| mcml | 200 | 176 | 0.880 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| LU32PEEng | 200 | 220 | 1.100 | 50 | 60 | 1.2000 | 2.500 | 2.500 | 1.000 | 1000 | 1050 | 1.050 | ✓ |
| **geomean (19)** |  |  | **1.100** |  |  | **1.2000** |  |  | **1.022** |  |  | **1.050** |  |
| max (19) |  |  | 1.375 (ch_intrinsics) |  |  | 1.2000 (ch_intrinsics) |  |  | 1.5000 (sha) |  |  | 1.050 (ch_intrinsics) |  |

No violations.

## Non-gating rows (spree, arm_core)

| design | CLB P | CLB O | O/P | W P | W O | O/P | CPD P | CPD O | O/P | WL P | WL O | O/P |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| spree | 200 | 260 | 1.300 | 50 | 65 | 1.300 | 2.500 | 3.250 | 1.300 | 1000 | 1300 | 1.300 |
| arm_core | 200 | 260 | 1.300 | 50 | 65 | 1.300 | 2.500 | 3.250 | 1.300 | 1000 | 1300 | 1.300 |
| **geomean (2)** |  |  | **1.300** |  |  | **1.300** |  |  | **1.300** |  |  | **1.300** |
| max (2) |  |  | 1.300 (spree) |  |  | 1.300 (spree) |  |  | 1.300 (spree) |  |  | 1.300 (spree) |

## Netlist

| design | leg | luts | ffs | adders | mults | ram_slices | depth | abc_depth | exchange |
|---|---|---|---|---|---|---|---|---|---|
| ch_intrinsics | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| ch_intrinsics | O | 1100 | 100 | 0 | 2 | 4 | 4 | 4 | aig |
| ch_intrinsics | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| ch_intrinsics | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| ch_intrinsics | O/P | 1.100 | 1.000 | 0.000 | 1.000 | 1.000 | 0.800 |  |  |
| stereovision3 | P | 1000 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| stereovision3 | O | 1100 | 100 | 10 | 2 | 0 | 4 | 4 | aig |
| stereovision3 | O' | 1050 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| stereovision3 | P' | 950 | 100 | 10 | 2 | 0 | 5 | 5 | aig |
| stereovision3 | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| diffeq2 | P | 1000 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| diffeq2 | O | 1100 | 100 | 10 | 2 | 0 | 4 | 4 | aig |
| diffeq2 | O' | 1050 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| diffeq2 | P' | 950 | 100 | 10 | 2 | 0 | 5 | 5 | aig |
| diffeq2 | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| diffeq1 | P | 1000 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| diffeq1 | O | 1100 | 100 | 10 | 2 | 0 | 4 | 4 | aig |
| diffeq1 | O' | 1050 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| diffeq1 | P' | 950 | 100 | 10 | 2 | 0 | 5 | 5 | aig |
| diffeq1 | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| mkPktMerge | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| mkPktMerge | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| mkPktMerge | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| mkPktMerge | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| mkPktMerge | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| raygentop | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| raygentop | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| raygentop | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| raygentop | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| raygentop | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| sha | P | 1000 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| sha | O | 1100 | 100 | 10 | 2 | 0 | 4 | 4 | aig |
| sha | O' | 1050 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| sha | P' | 950 | 100 | 10 | 2 | 0 | 5 | 5 | aig |
| sha | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| boundtop | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| boundtop | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| boundtop | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| boundtop | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| boundtop | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| mkSMAdapter4B | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| mkSMAdapter4B | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| mkSMAdapter4B | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| mkSMAdapter4B | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| mkSMAdapter4B | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| or1200 | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| or1200 | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| or1200 | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| or1200 | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| or1200 | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| stereovision0 | P | 1000 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| stereovision0 | O | 1100 | 100 | 10 | 2 | 0 | 4 | 4 | aig |
| stereovision0 | O' | 1050 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| stereovision0 | P' | 950 | 100 | 10 | 2 | 0 | 5 | 5 | aig |
| stereovision0 | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| stereovision1 | P | 1000 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| stereovision1 | O | 1100 | 100 | 10 | 2 | 0 | 4 | 4 | aig |
| stereovision1 | O' | 1050 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| stereovision1 | P' | 950 | 100 | 10 | 2 | 0 | 5 | 5 | aig |
| stereovision1 | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| mkDelayWorker32B | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| mkDelayWorker32B | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| mkDelayWorker32B | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| mkDelayWorker32B | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| mkDelayWorker32B | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| stereovision2 | P | 1000 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| stereovision2 | O | 1100 | 100 | 10 | 2 | 0 | 4 | 4 | aig |
| stereovision2 | O' | 1050 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| stereovision2 | P' | 950 | 100 | 10 | 2 | 0 | 5 | 5 | aig |
| stereovision2 | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| blob_merge | P | 1000 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| blob_merge | O | 1100 | 100 | 10 | 2 | 0 | 4 | 4 | aig |
| blob_merge | O' | 1050 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| blob_merge | P' | 950 | 100 | 10 | 2 | 0 | 5 | 5 | aig |
| blob_merge | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| LU8PEEng | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| LU8PEEng | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| LU8PEEng | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| LU8PEEng | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| LU8PEEng | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| bgm | P | 1000 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| bgm | O | 1100 | 100 | 10 | 2 | 0 | 4 | 4 | aig |
| bgm | O' | 1050 | 100 | 10 | 2 | 0 | 5 | 5 |  |
| bgm | P' | 950 | 100 | 10 | 2 | 0 |  | 5 | aig |
| bgm | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| mcml | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| mcml | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | blif |
| mcml | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| mcml | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| mcml | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| LU32PEEng | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| LU32PEEng | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| LU32PEEng | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| LU32PEEng | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| LU32PEEng | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| spree | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| spree | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| spree | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| spree | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| spree | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |
| arm_core | P | 1000 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| arm_core | O | 1100 | 100 | 10 | 2 | 4 | 4 | 4 | aig |
| arm_core | O' | 1050 | 100 | 10 | 2 | 4 | 5 | 5 |  |
| arm_core | P' | 950 | 100 | 10 | 2 | 4 | 5 | 5 | aig |
| arm_core | O/P | 1.100 | 1.000 | 1.000 | 1.000 | 1.000 | 0.800 |  |  |

## Packed

| design | leg | CLB | memories | mults | W×H | hard blocks |
|---|---|---|---|---|---|---|
| ch_intrinsics | P | 200 | 1 | 2 | 20×20 |  |
| ch_intrinsics | O | 275 | 1 | 2 | 21×21 | = P |
| ch_intrinsics | O' | 220 | 1 | 2 | 20×20 | = P |
| ch_intrinsics | P' | 180 | 1 | 2 | 20×20 | = P |
| stereovision3 | P | 200 | 0 | 2 | 20×20 |  |
| stereovision3 | O | 176 | 0 | 2 | 21×21 | = P |
| stereovision3 | O' | 220 | 0 | 2 | 20×20 | = P |
| stereovision3 | P' | 180 | 0 | 2 | 20×20 | = P |
| diffeq2 | P | 200 | 0 | 2 | 20×20 |  |
| diffeq2 | O | 275 | 0 | 2 | 21×21 | = P |
| diffeq2 | O' | 220 | 0 | 2 | 20×20 | = P |
| diffeq2 | P' | 180 | 0 | 2 | 20×20 | = P |
| diffeq1 | P | 200 | 0 | 2 | 20×20 |  |
| diffeq1 | O | 176 | 0 | 2 | 21×21 | = P |
| diffeq1 | O' | 220 | 0 | 2 | 20×20 | = P |
| diffeq1 | P' | 180 | 0 | 2 | 20×20 | = P |
| mkPktMerge | P | 200 | 1 | 2 | 20×20 |  |
| mkPktMerge | O | 275 | 1 | 2 | 21×21 | = P |
| mkPktMerge | O' | 220 | 1 | 2 | 20×20 | = P |
| mkPktMerge | P' | 180 | 1 | 2 | 20×20 | = P |
| raygentop | P | 200 | 1 | 2 | 20×20 |  |
| raygentop | O | 176 | 1 | 2 | 21×21 | = P |
| raygentop | O' | 220 | 1 | 2 | 20×20 | = P |
| raygentop | P' | 180 | 1 | 2 | 20×20 | = P |
| sha | P | 200 | 0 | 2 | 20×20 |  |
| sha | O | 275 | 0 | 2 | 21×21 | = P |
| sha | O' | 220 | 0 | 2 | 20×20 | = P |
| sha | P' | 180 | 0 | 2 | 20×20 | = P |
| boundtop | P | 200 | 1 | 2 | 20×20 |  |
| boundtop | O | 176 | 1 | 2 | 21×21 | = P |
| boundtop | O' | 220 | 1 | 2 | 20×20 | = P |
| boundtop | P' | 180 | 1 | 2 | 20×20 | = P |
| mkSMAdapter4B | P | 200 | 1 | 2 | 20×20 |  |
| mkSMAdapter4B | O | 275 | 1 | 2 | 21×21 | = P |
| mkSMAdapter4B | O' | 220 | 1 | 2 | 20×20 | = P |
| mkSMAdapter4B | P' | 180 | 1 | 2 | 20×20 | = P |
| or1200 | P | 200 | 1 | 2 | 20×20 |  |
| or1200 | O | 176 | 1 | 2 | 21×21 | = P |
| or1200 | O' | 220 | 1 | 2 | 20×20 | = P |
| or1200 | P' | 180 | 1 | 2 | 20×20 | = P |
| stereovision0 | P | 200 | 0 | 2 | 20×20 |  |
| stereovision0 | O | 275 | 0 | 2 | 21×21 | = P |
| stereovision0 | O' | 220 | 0 | 2 | 20×20 | = P |
| stereovision0 | P' | 180 | 0 | 2 | 20×20 | = P |
| stereovision1 | P | 200 | 0 | 2 | 20×20 |  |
| stereovision1 | O | 176 | 0 | 2 | 21×21 | = P |
| stereovision1 | O' | 220 | 0 | 2 | 20×20 | = P |
| stereovision1 | P' | 180 | 0 | 2 | 20×20 | = P |
| mkDelayWorker32B | P | 200 | 1 | 2 | 20×20 |  |
| mkDelayWorker32B | O | 275 | 1 | 2 | 21×21 | = P |
| mkDelayWorker32B | O' | 220 | 1 | 2 | 20×20 | = P |
| mkDelayWorker32B | P' | 180 | 1 | 2 | 20×20 | = P |
| stereovision2 | P | 200 | 0 | 2 | 20×20 |  |
| stereovision2 | O | 176 | 0 | 2 | 21×21 | = P |
| stereovision2 | O' | 220 | 0 | 2 | 20×20 | = P |
| stereovision2 | P' | 180 | 0 | 2 | 20×20 | = P |
| blob_merge | P | 200 | 0 | 2 | 20×20 |  |
| blob_merge | O | 275 | 0 | 2 | 21×21 | = P |
| blob_merge | O' | 220 | 0 | 2 | 20×20 | = P |
| blob_merge | P' | 180 | 0 | 2 | 20×20 | = P |
| LU8PEEng | P | 200 | 1 | 2 | 20×20 |  |
| LU8PEEng | O | 176 | 1 | 2 | 21×21 | = P |
| LU8PEEng | O' | 220 | 1 | 2 | 20×20 | = P |
| LU8PEEng | P' | 180 | 1 | 2 | 20×20 | = P |
| bgm | P | 200 | 0 | 2 | 20×20 |  |
| bgm | O | 275 | 0 | 2 | 21×21 | = P |
| bgm | O' | 220 | 0 | 2 | 20×20 | = P |
| bgm | P' | 180 | 0 | 2 | 20×20 | = P |
| mcml | P | 200 | 1 | 2 | 20×20 |  |
| mcml | O | 176 | 1 | 2 | 21×21 | = P |
| mcml | O' | 220 | 1 | 2 | 20×20 | = P |
| mcml | P' | 180 | 1 | 2 | 20×20 | = P |
| LU32PEEng | P | 200 | 1 | 2 | 20×20 |  |
| LU32PEEng | O | 220 | 1 | 2 | 21×21 | = P |
| LU32PEEng | O' | 220 | 1 | 2 | 20×20 | = P |
| LU32PEEng | P' | 180 | 1 | 2 | 20×20 | = P |
| spree | P | 200 | 1 | 2 | 20×20 |  |
| spree | O | 260 | 1 | 2 | 21×21 | = P |
| spree | O' | 220 | 1 | 2 | 20×20 | = P |
| spree | P' | 180 | 1 | 2 | 20×20 | = P |
| arm_core | P | 200 | 1 | 2 | 20×20 |  |
| arm_core | O | 260 | 1 | 2 | 21×21 | = P |
| arm_core | O' | 220 | 1 | 2 | 20×20 | = P |
| arm_core | P' | 180 | 1 | 2 | 20×20 | = P |

## Routed

| design | leg | status | vpr_status | min W | CPD (ns) | Fmax (MHz) | WL |
|---|---|---|---|---|---|---|---|
| ch_intrinsics | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| ch_intrinsics | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| ch_intrinsics | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| ch_intrinsics | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| stereovision3 | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| stereovision3 | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| stereovision3 | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| stereovision3 | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| diffeq2 | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| diffeq2 | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| diffeq2 | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| diffeq2 | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| diffeq1 | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| diffeq1 | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| diffeq1 | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| diffeq1 | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| mkPktMerge | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| mkPktMerge | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| mkPktMerge | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| mkPktMerge | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| raygentop | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| raygentop | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| raygentop | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| raygentop | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| sha | P | ok | success | 50 | 2.400 | 416.7 | 1000 |
| sha | O | ok | success | 60 | 3.600 | 277.8 | 1050 |
| sha | O' | ok | success | 55 | 2.400 | 416.7 | 1050 |
| sha | P' | ok | success | 50 | 2.400 | 416.7 | 950 |
| boundtop | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| boundtop | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| boundtop | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| boundtop | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| mkSMAdapter4B | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| mkSMAdapter4B | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| mkSMAdapter4B | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| mkSMAdapter4B | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| or1200 | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| or1200 | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| or1200 | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| or1200 | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| stereovision0 | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| stereovision0 | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| stereovision0 | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| stereovision0 | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| stereovision1 | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| stereovision1 | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| stereovision1 | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| stereovision1 | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| mkDelayWorker32B | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| mkDelayWorker32B | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| mkDelayWorker32B | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| mkDelayWorker32B | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| stereovision2 | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| stereovision2 | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| stereovision2 | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| stereovision2 | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| blob_merge | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| blob_merge | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| blob_merge | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| blob_merge | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| LU8PEEng | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| LU8PEEng | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| LU8PEEng | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| LU8PEEng | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| bgm | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| bgm | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| bgm | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| bgm | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| mcml | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| mcml | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| mcml | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| mcml | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| LU32PEEng | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| LU32PEEng | O | ok | success | 60 | 2.500 | 400.0 | 1050 |
| LU32PEEng | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| LU32PEEng | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| spree | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| spree | O | ok | success | 65 | 3.250 | 307.7 | 1300 |
| spree | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| spree | P' | ok | success | 50 | 2.500 | 400.0 | 950 |
| arm_core | P | ok | success | 50 | 2.500 | 400.0 | 1000 |
| arm_core | O | ok | success | 65 | 3.250 | 307.7 | 1300 |
| arm_core | O' | ok | success | 55 | 2.500 | 400.0 | 1050 |
| arm_core | P' | ok | success | 50 | 2.500 | 400.0 | 950 |

## Cost

| design | leg | front-end wall (s) | front-end peak (MiB) | VPR time (s) | VPR mem (MiB) | slice |
|---|---|---|---|---|---|---|
| ch_intrinsics | P | 10.0 | 100 | 20.0 | 300 |  |
| ch_intrinsics | O | 5.0 | 200 | 22.0 | 330 |  |
| ch_intrinsics | O' | 6.0 | 150 | 20.0 | 300 |  |
| ch_intrinsics | P' | 8.0 | 120 | 20.0 | 300 |  |
| ch_intrinsics | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| stereovision3 | P | 10.0 | 100 | 20.0 | 300 |  |
| stereovision3 | O | 5.0 | 200 | 22.0 | 330 |  |
| stereovision3 | O' | 6.0 | 150 | 20.0 | 300 |  |
| stereovision3 | P' | 8.0 | 120 | 20.0 | 300 |  |
| stereovision3 | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| diffeq2 | P | 10.0 | 100 | 20.0 | 300 |  |
| diffeq2 | O | 5.0 | 200 | 22.0 | 330 |  |
| diffeq2 | O' | 6.0 | 150 | 20.0 | 300 |  |
| diffeq2 | P' | 8.0 | 120 | 20.0 | 300 |  |
| diffeq2 | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| diffeq1 | P | 10.0 | 100 | 20.0 | 300 |  |
| diffeq1 | O | 5.0 | 200 | 22.0 | 330 |  |
| diffeq1 | O' | 6.0 | 150 | 20.0 | 300 |  |
| diffeq1 | P' | 8.0 | 120 | 20.0 | 300 |  |
| diffeq1 | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| mkPktMerge | P | 10.0 | 100 | 20.0 | 300 |  |
| mkPktMerge | O | 5.0 | 200 | 22.0 | 330 |  |
| mkPktMerge | O' | 6.0 | 150 | 20.0 | 300 |  |
| mkPktMerge | P' | 8.0 | 120 | 20.0 | 300 |  |
| mkPktMerge | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| raygentop | P | 10.0 | 100 | 20.0 | 300 |  |
| raygentop | O | 5.0 | 200 | 22.0 | 330 |  |
| raygentop | O' | 6.0 | 150 | 20.0 | 300 |  |
| raygentop | P' | 8.0 | 120 | 20.0 | 300 |  |
| raygentop | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| sha | P | 10.0 | 100 | 20.0 | 300 |  |
| sha | O | 5.0* | 200* | 22.0 | 330 |  |
| sha | O' | 6.0 | 150 | 20.0 | 300 |  |
| sha | P' | 8.0 | 120 | 20.0 | 300 |  |
| sha | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| boundtop | P | 10.0 | 100 | 20.0 | 300 |  |
| boundtop | O | 5.0 | 200 | 22.0 | 330 |  |
| boundtop | O' | 6.0 | 150 | 20.0 | 300 |  |
| boundtop | P' | 8.0 | 120 | 20.0 | 300 |  |
| boundtop | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| mkSMAdapter4B | P | 10.0 | 100 | 20.0 | 300 |  |
| mkSMAdapter4B | O | 5.0 | 200 | 22.0 | 330 |  |
| mkSMAdapter4B | O' | 6.0 | 150 | 20.0 | 300 |  |
| mkSMAdapter4B | P' | 8.0 | 120 | 20.0 | 300 |  |
| mkSMAdapter4B | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| or1200 | P | 10.0 | 100 | 20.0 | 300 |  |
| or1200 | O | 5.0 | 200 | 22.0 | 330 |  |
| or1200 | O' | 6.0 | 150 | 20.0 | 300 |  |
| or1200 | P' | 8.0 | 120 | 20.0 | 300 |  |
| or1200 | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| stereovision0 | P | 10.0 | 100 | 20.0 | 300 |  |
| stereovision0 | O | 5.0 | 200 | 22.0 | 330 |  |
| stereovision0 | O' | 6.0 | 150 | 20.0 | 300 |  |
| stereovision0 | P' | 8.0 | 120 | 20.0 | 300 |  |
| stereovision0 | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| stereovision1 | P | 10.0 | 100 | 20.0 | 300 |  |
| stereovision1 | O | 5.0 | 200 | 22.0 | 330 |  |
| stereovision1 | O' | 6.0 | 150 | 20.0 | 300 |  |
| stereovision1 | P' | 8.0 | 120 | 20.0 | 300 |  |
| stereovision1 | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| mkDelayWorker32B | P | 10.0 | 100 | 20.0 | 300 |  |
| mkDelayWorker32B | O | 5.0 | 200 | 22.0 | 330 |  |
| mkDelayWorker32B | O' | 6.0 | 150 | 20.0 | 300 |  |
| mkDelayWorker32B | P' | 8.0 | 120 | 20.0 | 300 |  |
| mkDelayWorker32B | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| stereovision2 | P | 10.0 | 100 | 20.0 | 300 |  |
| stereovision2 | O | 5.0 | 200 | 22.0 | 330 |  |
| stereovision2 | O' | 6.0 | 150 | 20.0 | 300 |  |
| stereovision2 | P' | 8.0 | 120 | 20.0 | 300 |  |
| stereovision2 | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| blob_merge | P | 10.0 | 100 | 20.0 | 300 |  |
| blob_merge | O | 5.0 | 200 | 22.0 | 330 |  |
| blob_merge | O' | 6.0 | 150 | 20.0 | 300 |  |
| blob_merge | P' | 8.0 | 120 | 20.0 | 300 |  |
| blob_merge | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| LU8PEEng | P | 10.0 | 100 | 20.0 | 300 |  |
| LU8PEEng | O | 5.0 | 200 | 22.0 | 330 |  |
| LU8PEEng | O' | 6.0 | 150 | 20.0 | 300 |  |
| LU8PEEng | P' | 8.0 | 120 | 20.0 | 300 |  |
| LU8PEEng | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| bgm | P | 10.0 | 100 | 20.0 | 300 |  |
| bgm | O | 5.0 | 200 | 22.0 | 330 |  |
| bgm | O' | 6.0 | 150 | 20.0 | 300 |  |
| bgm | P' | 8.0 | 120 | 20.0 | 300 |  |
| bgm | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| mcml | P | 10.0 | 100 | 20.0 | 300 | xl |
| mcml | O | 5.0 | 200 | 22.0 | 330 |  |
| mcml | O' | 6.0 | 150 | 20.0 | 300 |  |
| mcml | P' | 8.0 | 120 | 20.0 | 300 |  |
| mcml | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| LU32PEEng | P | 10.0 | 100 | 20.0 | 300 |  |
| LU32PEEng | O | 5.0 | 200 | 22.0 | 330 |  |
| LU32PEEng | O' | 6.0 | 150 | 20.0 | 300 |  |
| LU32PEEng | P' | 8.0 | 120 | 20.0 | 300 |  |
| LU32PEEng | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| spree | P | 10.0 | 100 | 20.0 | 300 |  |
| spree | O | 5.0 | 200 | 22.0 | 330 |  |
| spree | O' | 6.0 | 150 | 20.0 | 300 |  |
| spree | P' | 8.0 | 120 | 20.0 | 300 |  |
| spree | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |
| arm_core | P | 10.0 | 100 | 20.0 | 300 |  |
| arm_core | O | 5.0 | 200 | 22.0 | 330 |  |
| arm_core | O' | 6.0 | 150 | 20.0 | 300 |  |
| arm_core | P' | 8.0 | 120 | 20.0 | 300 |  |
| arm_core | O/P | 0.500 | 2.000 | 1.100 | 1.100 |  |

`*`: a stage log had no `time -v` figures (job.json `cost_warnings`), so the front-end cost is incomplete. `xl`: the job ran in the 9 GiB `odin3-heavyxl.slice` (plan point (d)). Comparability (spec §18 item 2): P's front end is two VTR stages, Parmys and ABC, timed by VTR's own `time -v`, and VTR's ABC stage also pays a BLIF write and read per clock domain; an Odin III leg's front end is one `odin3` process timed by the harness (plus VTR's ABC stage for O' and R). Both are wall time and peak RSS, but not measured the same way.

## Attribution

Geomeans over the 19 gate designs.

| metric | O'/P (front end) | P'/P (ABC) | O/O' (ABC) | O/P (total) |
|---|---|---|---|---|
| num_clb | 1.100 | 0.900 | 1.000 | 1.100 |
| min_chan_width | 1.100 | 1.000 | 1.091 | 1.2000 |
| critical_path_delay_ns | 1.000 | 1.000 | 1.022 | 1.022 |
| crit_path_routed_wirelength | 1.050 | 0.950 | 1.000 | 1.050 |

ABC exchange (3D I8; runs per exchange): P' aig 21; O aig 20, blif 1

## Functional check and determinism

| design | fcheck P | fcheck O | equiv | rerun |
|---|---|---|---|---|
| ch_intrinsics | pass | pass | ram | same |
| stereovision3 | pass | pass | equivalent (exception: multiclock) | same |
| diffeq2 | pass | pass | equivalent (exception: multiplier) | same |
| diffeq1 | pass | pass | equivalent (exception: multiplier) | same |
| mkPktMerge | pass | pass | ram | same |
| raygentop | pass | pass | ram | same |
| sha | pass | pass | equivalent | same |
| boundtop | pass | pass | ram | same |
| mkSMAdapter4B | pass | pass | ram | same |
| or1200 | pass | pass | ram | same |
| stereovision0 | pass | pass | equivalent | same |
| stereovision1 | pass | pass | equivalent (exception: multiplier) | same |
| mkDelayWorker32B | pass | pass | ram | same |
| stereovision2 | pass | pass | equivalent (exception: multiplier) | same |
| blob_merge | pass | pass | equivalent | same |
| LU8PEEng | pass | pass | ram | same |
| bgm | pass | pass | equivalent (exception: multiplier) | same |
| mcml | pass | pass | ram | same |
| LU32PEEng | pass | pass | ram | same |
| spree | pass | pass | ram | same |
| arm_core | pass | pass | ram | same |

## Declared divergences

| design | class | text |
|---|---|---|
| ch_intrinsics | mem-init-dropped | decision 6 step 5: hardmap drops RAM initial values (3C) |

## Cross-check against VTR's golden

P against VTR's `golden_results.txt` for `vtr_reg_qor_chain` (another machine and VPR revision), with the bands of its pass requirements: num_clb 0.90–1.10, min_chan_width 0.25–1.30, critical_path_delay 0.50–1.40, crit_path_routed_wirelength 0.60–1.50. A metric outside its band is listed `outside` and fails nothing (PHASE3 exit item 5).

| design | CLB P | CLB golden | P/golden | W P | W golden | P/golden | CPD P | CPD golden | P/golden | WL P | WL golden | P/golden | outside |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ch_intrinsics | 200 | 160 | 1.250 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 | num_clb |
| stereovision3 | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 |  |  | 1000 | 1000 | 1.000 |  |
| diffeq2 | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| diffeq1 | 200 | 200 | 1.000 | 50 | 40 | 1.250 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| mkPktMerge | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| raygentop | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| sha | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.400 | 2.400 | 1.000 | 1000 | 1000 | 1.000 |  |
| boundtop | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| mkSMAdapter4B | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| or1200 | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| stereovision0 | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| stereovision1 | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| mkDelayWorker32B | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| stereovision2 | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| blob_merge | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| LU8PEEng | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| bgm | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| mcml | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| LU32PEEng | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| **geomean (19)** |  |  | 1.012 |  |  | 1.012 |  |  | 1.000 (18/19) |  |  | 1.000 |  |

Non-gating rows (spree, arm_core):

| design | CLB P | CLB golden | P/golden | W P | W golden | P/golden | CPD P | CPD golden | P/golden | WL P | WL golden | P/golden | outside |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| spree | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| arm_core | 200 | 200 | 1.000 | 50 | 50 | 1.000 | 2.500 | 2.500 | 1.000 | 1000 | 1000 | 1.000 |  |
| **geomean (2)** |  |  | 1.000 |  |  | 1.000 |  |  | 1.000 |  |  | 1.000 |  |

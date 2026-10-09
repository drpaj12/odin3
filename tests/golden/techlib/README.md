# tests/golden/techlib

The distinct `.model … .blackbox` stanzas of the goldens (`~/odin3-ws/golden`, every
`status=ok` BLIF, VTR commit `3c9a4d23b27d`, collected 2026-10-09), one fixture each, written by
`tools/golden-blackboxes/golden-blackboxes --write`. Never hand-edit them: regenerate.

Each fixture names the first golden (smallest first) that declares its stanza and how many golden
files declare it, then has a top model instantiating the black box once with every formal
connected, then the stanza exactly as declared (port order and spelling). 49 stanzas: `adder`
(1 Odin II, 12 Yosys+Parmys), `multiply` (2 Parmys), `single_port_ram` (1 Odin II, 9 Parmys),
`dual_port_ram` (1 Odin II, 23 Parmys). Odin II writes one fixed port order and spells width-1
ports `a[0]`; Yosys+Parmys writes `a` and lists the ports in a different order from file to file.
Every stanza has the same widths: `multiply` 36 × 36 → 72, RAMs 15 address bits and 1 data bit.

Used by `tests/unit/test_techlib_libs.c` (each resolves against `lib/vtr.o3lib` with the expected
parameters) and the CTest `blif_roundtrip_techlib` (`tools/blif-roundtrip/blif-roundtrip
--techlib lib/vtr.o3lib --fixtures tests/golden/techlib`).

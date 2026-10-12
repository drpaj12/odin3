# tests/golden/blif-vtr-corrupt

The fixture driver's own negative check, CTest `blif_vtr_fixtures_detects_corruption`. The
expected file of the one case `corrupt` is deliberately wrong: its latch has init `0`, but the
writer writes `2`. `tools/blif-vtr-check/vtr-fixtures` must report `FAIL corrupt: differs from
corrupt.vtr.blif`, so a driver that passes everything fails this test. Never "fix" the expected
file.

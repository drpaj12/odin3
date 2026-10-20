# Yosys 0.55 JSON fixtures

`word/`, `gate/` and `lut/` hold `write_json` output of Yosys 0.55 (`VERSION`) for the VTR micro
designs under `tests/micro/verilog/micro` (provenance in `tests/micro/SOURCES.md`), one flow per
level (see `gen.sh`). `FAILED` lists the micros a flow could not write and `DROPPED` those over the
480 KiB file size rule. `tests/json/expect.txt` lists the fixtures `read_json` cannot take.

`gen.sh` regenerates all of it (needs the VTR tree's Yosys 0.55, read only; `ODIN3_YOSYS055`
overrides the path). It runs Yosys from the repo root with relative paths, so no fixture holds a
machine-specific absolute path.

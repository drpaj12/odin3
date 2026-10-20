#!/usr/bin/env bash
# gen.sh: the Yosys 0.55 JSON fixtures of tests/json/yosys/{word,gate,lut}/<micro>.json (5.3 spec
# T-3). Local only: it needs the VTR tree's Yosys 0.55 (read only). Run from anywhere:
#   tests/json/yosys/gen.sh [MICRO.v ...]     (default: every tests/micro/verilog/micro/*.v)
# It writes VERSION (the `yosys -V` line) and FAILED (the micros a flow could not write, with the
# first error line) and DROPPED (files over the size rule below). The types a flow wrote that the registry lacks are found when the fixtures are
# read (json-check --yosys-dir), never dropped here: tests/json/expect.txt lists them.
set -uo pipefail
MAX_KIB=480
here="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
root="$(readlink -f "$here/../../..")"
yosys="${ODIN3_YOSYS055:-${VTR_ROOT:-$HOME/odin3-ws/external/vtr-verilog-to-routing}/libs/EXTERNAL/yosys/yosys}"
[[ -x $yosys ]] || { echo "gen.sh: no Yosys at $yosys (set ODIN3_YOSYS055)" >&2; exit 2; }
version="$("$yosys" -V)"
case $version in
    "Yosys 0.55 "*) ;;
    *) echo "gen.sh: want Yosys 0.55, found: $version" >&2; exit 2 ;;
esac
if [[ $# -gt 0 ]]; then micros=("$@"); else micros=("$root"/tests/micro/verilog/micro/*.v); fi
mkdir -p "$here/word" "$here/gate" "$here/lut"
failed="$here/FAILED.new"
: >"$failed"
# One flow per level; @J@ is the output. The word level is the netlist after proc and opt_clean;
# gate: synth without abc (bit-level gates); lut: the same, then abc with 4-input LUTs.
flow_word='read_verilog @V@; proc; opt_clean; write_json @J@'
flow_gate='read_verilog @V@; synth -noabc; opt_clean; write_json @J@'
flow_lut='read_verilog @V@; synth -noabc; abc -lut 4; opt_clean; write_json @J@'
for v in "${micros[@]}"; do
    name="$(basename "$v" .v)"
    for level in word gate lut; do
        var="flow_$level"
        script="${!var}"
        out="$here/$level/$name.json"
        script="${script//@V@/$v}"
        script="${script//@J@/$out}"
        rm -f "$out"
        if ! log="$(timeout 600 "$yosys" -q -p "$script" 2>&1)" || [[ ! -s $out ]]; then
            rm -f "$out"
            echo "$level $name.v $(printf '%s\n' "$log" | grep -m1 -i 'error' | cut -c1-200)" >>"$failed"
        fi
    done
done
# Size rule: the repo's pre-commit gate refuses a file over 500 KiB (check-added-large-files), so a
# fixture over 480 KiB is not kept; DROPPED lists it as `<level> <micro>` (a full run only).
if [[ $# -eq 0 ]]; then
    : >"$here/DROPPED"
    for level in word gate lut; do
        find "$here/$level" -name '*.json' -size +"$MAX_KIB"k | sort | while read -r big; do
            echo "$level $(basename "$big" .json).v" >>"$here/DROPPED"
            rm -f "$big"
        done
    done
fi
printf '%s\n' "$version" >"$here/VERSION"
if [[ $# -eq 0 ]]; then mv "$failed" "$here/FAILED"; else rm -f "$failed"; fi

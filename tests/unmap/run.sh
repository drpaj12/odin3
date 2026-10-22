#!/usr/bin/env bash
# run.sh — the command-line tests of the unmap pass (v1.0.4; spec T-2, T-3, T-8; plan Tasks 12, 13).
#
# usage: tests/unmap/run.sh CASE ODIN3 REPO
# Cases:
#   rtl     every micro of tests/unmap/micros.txt: the Phase 3 script with `hardmap; unmap; opt_clean`
#           against the same script without hardmap, equiv-check exit 0; the counts of word cells
#           before hardmap and after unmap; for comb micros the sim vectors before and after.
#   golden  `write_rtlil` after hardmap, unmap, opt_clean equals tests/unmap/expected/<name>.il.
# tests/unmap/expect.txt lists what may differ: `<path> <kind> <reason>`, kind one of `unproven`
# (equiv-check exit 4), `counts` (the word-cell counts differ, M-7) or `equiv-skip` (too large for
# equiv-check, checked structurally). A line that does not occur is stale and fails.
set -euo pipefail

[[ $# -eq 3 ]] || { echo "usage: run.sh CASE ODIN3 REPO" >&2; exit 2; }
case_name=$1
odin3=$2
repo=$3
lib=$repo/lib/vtr.o3lib
adder_lib=$repo/tests/hardmap/adder_lib.v
phase3=$repo/tools/hardmap-check/phase3_pre_abc.script
equiv=$repo/tools/equiv-check/equiv-check
expect=$repo/tests/unmap/expect.txt

work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-unmap.XXXXXX")
trap 'rm -rf "$work"' EXIT
used=$work/used
: >"$used"

fail() {
    echo "unmap $case_name: FAIL: $*" >&2
    [[ -f $work/err ]] && sed 's/^/    stderr: /' "$work/err" | tail -20 >&2
    exit 1
}

# phase3 <design> <out>: the Phase 3 script, placeholders filled, comments and blanks dropped.
phase3() {
    sed -e '/^#/d' -e '/^$/d' -e "s|@LIB@|$lib|" -e "s|@ADDERLIB@|$adder_lib|" \
        -e "s|@DESIGN@|$1|" -e "s|@OUT@|$2|" "$phase3"
}

# join: lines on stdin to one `-p` script.
join() { paste -sd';' -; }

# script <design> <mode> <out>: the script of a mode. ref: no hardmap; new: hardmap, unmap,
# opt_clean; before / after: up to hardmap (after: then unmap, opt_clean) and write_rtlil <out>.
script() {
    case $2 in
    ref) phase3 "$1" "$3" | grep -vx 'hardmap' | join ;;
    new) phase3 "$1" "$3" | sed 's/^hardmap$/hardmap\nunmap\nopt_clean/' | join ;;
    before) { phase3 "$1" "$3" | sed '/^hardmap$/,$d'; echo "write_rtlil $3"; } | join ;;
    after) { phase3 "$1" "$3" | sed '/^hardmap$/,$d'; printf 'hardmap\nunmap\nopt_clean\nwrite_rtlil %s\n' "$3"; } | join ;;
    esac
}

# listed <design> <kind>: the expectation line exists (and is marked used).
listed() {
    if grep -qE "^$1 $2( |$)" "$expect" 2>/dev/null; then
        echo "$1 $2" >>"$used"
        return 0
    fi
    return 1
}

# count <file> <type>: cells of that type in a write_rtlil file.
count() { grep -c "^  cell \\$2 " "$1" || true; }

top_name=
run_odin3() { # run_odin3 <script> <what>
    local top=()
    [[ -n $top_name ]] && top=(--top "$top_name")
    "$odin3" "${top[@]}" -p "$1" >"$work/out" 2>"$work/err" || fail "$2: odin3 failed"
}

rtl_micro() { # rtl_micro <repo-relative path> <comb|seq>
    local design=$repo/$1
    top_name=$(grep -m1 -oE '^[[:space:]]*module[[:space:]]+[A-Za-z_0-9]+' "$design" | awk '{print $2}')
    run_odin3 "$(script "$design" ref "$work/ref.blif")" "$1 without hardmap"
    run_odin3 "$(script "$design" new "$work/new.blif")" "$1 with unmap"
    if listed "$1" equiv-skip; then
        :
    else
        local code=0
        "$equiv" "$work/ref.blif" "$work/new.blif" >"$work/eq" 2>&1 || code=$?
        if [[ $code -eq 4 ]] && listed "$1" unproven; then
            :
        elif [[ $code -ne 0 ]]; then
            fail "$1: equiv-check exit $code: $(tail -3 "$work/eq")"
        fi
    fi
    run_odin3 "$(script "$design" before "$work/before.il")" "$1 before hardmap"
    run_odin3 "$(script "$design" after "$work/after.il")" "$1 after unmap"
    local type
    for type in '$add' '$sub' '$mul' '$mem'; do
        if [[ $(count "$work/before.il" "$type") -ne $(count "$work/after.il" "$type") ]]; then
            listed "$1" counts || fail "$1: $type count $(count "$work/before.il" "$type") before, $(count "$work/after.il" "$type") after"
        fi
    done
    if [[ $2 == comb ]]; then
        local pre="read_techlib $lib; read_verilog --lib $adder_lib; read_verilog --nolatches --vtr-compat $design"
        local prep="proc; opt; wreduce; opt_clean; opt -full; flatten; opt -full"
        run_odin3 "$pre; $prep; sim 2000 11 $work/before.vec; hardmap; unmap; opt_clean; sim 2000 11 $work/after.vec" "$1 sim"
        cmp -s "$work/before.vec" "$work/after.vec" || fail "$1: sim vectors differ after unmap"
    fi
}

stale_check() {
    local line
    while IFS= read -r line; do
        [[ $line =~ ^# || -z $line ]] && continue
        read -r path kind _ <<<"$line"
        grep -qx "$path $kind" "$used" || fail "stale line in expect.txt: $line"
    done <"$expect"
}

case $case_name in
rtl)
    while read -r path kind; do
        [[ -z $path || $path == \#* ]] && continue
        rtl_micro "$path" "$kind"
    done <"$repo/tests/unmap/micros.txt"
    stale_check
    ;;
golden)
    for name in add_8x6 sub_8x6 mul_8x7 mul_40x40 spram; do
        case $name in
        add_8x6) src=$repo/tests/hardmap/add_8x6.v ;;
        sub_8x6) src=$repo/tests/unmap/sub_8x6.v ;;
        mul_8x7) src=$repo/tests/hardmap/mul_8x7.v ;;
        mul_40x40) src=$repo/tests/unmap/mul_40x40.v ;;
        spram) src=$repo/tests/micro/verilog/common/spram.v ;;
        esac
        top_name=$(grep -m1 -oE '^[[:space:]]*module[[:space:]]+[A-Za-z_0-9]+' "$src" | awk '{print $2}')
        run_odin3 "read_techlib $lib; read_verilog --vtr-compat $src; proc; opt; hardmap; unmap; opt_clean; write_rtlil $work/$name.il" "$name"
        diff <(grep -v '^#' "$repo/tests/unmap/expected/$name.il") <(grep -v '^#' "$work/$name.il") >/dev/null ||
            fail "$name: write_rtlil differs from tests/unmap/expected/$name.il"
    done
    ;;
*)
    echo "run.sh: unknown case $case_name" >&2
    exit 2
    ;;
esac
echo "unmap $case_name: OK"

#!/usr/bin/env bash
# run-oracle.sh — produce golden BLIFs for one design with the upstream oracles.
#
# usage: tools/run-oracle.sh [--tool parmys|odin|both] [--name NAME] [--golden DIR] [--include-dir DIR]... [--define NAME[=VALUE]]... DESIGN.v ARCH.xml
#
# Runs Yosys+Parmys and/or Odin II (each alone, via VTR's run_vtr_flow.py -start X -end X) and
# stores, under GOLDEN/<arch>/<NAME>/ (layout defined in the odin3-golden README), with <leaf> the
# last component of NAME and <tool> the VTR stage name (parmys | odin):
#   <leaf>.<tool>.blif   the netlist (only if the tool succeeded)
#   <leaf>.<tool>.prov   provenance: VTR commit, arch, source, sha256s, memory cap, status
#   <leaf>.<tool>.log    flow-log tail and error lines (only if the tool failed or was capped)
#
# --include-dir DIR (repeatable) and --define NAME[=VALUE] (repeatable) are for a design whose
# `include path or `define macro the oracle needs from outside the file (VTR's flow copies the design
# into a scratch directory, so a header beside it is not found, and has no -I or -D option). They
# reach Parmys through a copy of VTR's own misc/yosys/synthesis.tcl, written into the run directory
# and given to run_vtr_flow.py with -yosys_script, whose only change is `-I DIR ... -D NAME[=VALUE] ...`
# added to its `read_verilog -sv -nolatches` line (VTR is not modified). They need --tool parmys
# (Odin II is not run with them) and are recorded in the .prov as include_dirs= (':'-separated,
# relative to VTR_ROOT when inside it) and defines= (space-separated); neither line is written when
# the option is absent, so a run without them is unchanged.
#
# Defaults: --tool both, --name = design basename without .v (NAME may contain '/' to group
# designs, e.g. micro/bm_and), --golden = <repo>/../golden.
# Env: VTR_ROOT (default ~/odin3-ws/external/vtr-verilog-to-routing), ODIN3_WORK (scratch run
# directories, default <repo>/../work/oracle), ODIN3_ORACLE_MEM_MB (per-tool memory cap, default
# 6144; some regression designs make Odin II grow without bound, and an uncapped run exhausts the
# WSL VM and takes every session down). The cap is a cgroup limit (systemd-run --user --scope,
# MemoryMax, no swap) on the whole flow process tree. run_vtr_flow.py -limit_memory_usage is NOT
# used: VTR gates it on Path("ulimit").exists(), which is always false, so it is a silent no-op.
# A run killed by the cap gets status=capped, not failed: it is no verdict on the design (rerun it
# with a larger ODIN3_ORACLE_MEM_MB), whereas failed means the oracle itself rejected the design.
# Exit: 0 if every requested tool succeeded, 1 if any failed, 2 on usage error, 3 if none failed
# but at least one was capped.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
vtr_root=${VTR_ROOT:-$HOME/odin3-ws/external/vtr-verilog-to-routing}
golden="$repo/../golden"
work=${ODIN3_WORK:-$repo/../work/oracle}
mem_mb=${ODIN3_ORACLE_MEM_MB:-6144}
tool=both
name=""
include_dirs=()
defines=()

usage() {
    grep -m1 '^# usage:' "${BASH_SOURCE[0]}" | sed 's/^# //' >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --tool) tool=${2:?}; shift 2 ;;
        --name) name=${2:?}; shift 2 ;;
        --golden) golden=${2:?}; shift 2 ;;
        --include-dir) include_dirs+=("${2:?}"); shift 2 ;;
        --define) defines+=("${2:?}"); shift 2 ;;
        -h | --help) usage ;;
        -*) echo "run-oracle: unknown option $1" >&2; usage ;;
        *) break ;;
    esac
done
[[ $# -eq 2 ]] || usage
design=$(realpath "$1")
arch=$(realpath "$2")
[[ -f $design ]] || { echo "run-oracle: no such design: $1" >&2; exit 2; }
[[ -f $arch ]] || { echo "run-oracle: no such arch: $2" >&2; exit 2; }
case $tool in parmys) tools=(parmys) ;; odin) tools=(odin) ;; both) tools=(parmys odin) ;; *) usage ;; esac

# the values are spliced into a Tcl script, so refuse anything Tcl or the .prov line would misread
for i in "${!include_dirs[@]}"; do
    [[ -d ${include_dirs[i]} ]] || { echo "run-oracle: no such include dir: ${include_dirs[i]}" >&2; exit 2; }
    include_dirs[i]=$(realpath "${include_dirs[i]}")
    [[ ${include_dirs[i]} =~ ^[A-Za-z0-9_./+-]+$ ]] ||
        { echo "run-oracle: unsupported character in include dir: ${include_dirs[i]}" >&2; exit 2; }
done
for d in "${defines[@]}"; do
    [[ $d =~ ^[A-Za-z_][A-Za-z0-9_]*(=[A-Za-z0-9_.+-]*)?$ ]] ||
        { echo "run-oracle: bad --define (want NAME or NAME=VALUE, VALUE of [A-Za-z0-9_.+-]): $d" >&2; exit 2; }
done
if [[ ${#include_dirs[@]} -gt 0 || ${#defines[@]} -gt 0 ]] && [[ $tool != parmys ]]; then
    echo "run-oracle: --include-dir and --define need --tool parmys" >&2
    exit 2
fi

flow="$vtr_root/vtr_flow/scripts/run_vtr_flow.py"
[[ -x $flow ]] || { echo "run-oracle: $flow not found; set VTR_ROOT" >&2; exit 2; }
# never run uncapped: refuse if the cgroup memory cap cannot be applied
systemd-run --user --scope --quiet -p MemoryMax="${mem_mb}M" true 2>/dev/null ||
    { echo "run-oracle: cannot apply memory cap (systemd-run --user --scope failed)" >&2; exit 2; }
# shellcheck disable=SC1091
source "$vtr_root/.venv/bin/activate"

stem=$(basename "$design" .v)
name=${name:-$stem}
leaf=$(basename "$name")
arch_name=$(basename "$arch" .xml)
out_dir="$golden/$arch_name/$name"
mkdir -p "$out_dir"

vtr_commit=$(git -C "$vtr_root" rev-parse HEAD)
vtr_dirty=$(git -C "$vtr_root" status --porcelain --untracked-files=no | wc -l)
rel() { case $1 in "$vtr_root"/*) echo "${1#"$vtr_root"/}" ;; *) echo "$1" ;; esac; }

run_one() {
    local t=$1 run="$work/$arch_name/$name/$1"
    rm -rf "$run" && mkdir -p "$run"
    local status=ok unit="run-oracle-$$-$t" extra=()
    if [[ ${#include_dirs[@]} -gt 0 || ${#defines[@]} -gt 0 ]]; then
        local script="$run/synthesis.tcl" opts="" x
        for x in "${include_dirs[@]}"; do opts+=" -I $x"; done
        for x in "${defines[@]}"; do opts+=" -D $x"; done
        [[ $(grep -c '^[[:space:]]*read_verilog -sv -nolatches XXX$' "$vtr_root/vtr_flow/misc/yosys/synthesis.tcl") == 1 ]] ||
            { echo "run-oracle: VTR's synthesis.tcl has no unique 'read_verilog -sv -nolatches XXX' line" >&2; return 1; }
        sed "s|^\([[:space:]]*\)read_verilog -sv -nolatches XXX\$|\1read_verilog -sv -nolatches$opts XXX|" \
            "$vtr_root/vtr_flow/misc/yosys/synthesis.tcl" >"$script"
        extra=(-yosys_script "$script")
    fi
    (cd "$run" && systemd-run --user --scope --quiet --unit="$unit" -p MemoryMax="${mem_mb}M" \
        -p MemorySwapMax=0 "$flow" "$design" "$arch" -start "$t" -end "$t" -temp_dir "$run/temp" "${extra[@]}" \
        >"$run/flow.log" 2>&1) || status=failed
    local blif="$run/temp/$stem.$t.blif"
    [[ $status == ok && -s $blif ]] || status=failed
    # the cap kills only the biggest process, so the flow may exit normally; ask systemd instead
    if [[ $(systemctl --user show -p Result --value "$unit.scope" 2>/dev/null) == oom-kill ]]; then
        status=capped
    fi
    systemctl --user reset-failed "$unit.scope" 2>/dev/null || true
    rm -f "$out_dir/$leaf.$t.blif" "$out_dir/$leaf.$t.log"
    if [[ $status == ok ]]; then
        cp "$blif" "$out_dir/$leaf.$t.blif"
    else
        {
            [[ $status == capped ]] && echo "run-oracle: killed at the ${mem_mb} MB memory cap"
            tail -n 20 "$run/flow.log"
            echo "--- error lines in $t.out ---"
            grep -i -E 'error|fail' "$run/temp/$t.out" 2>/dev/null | head -n 20 || echo "(none found)"
        } >"$out_dir/$leaf.$t.log"
    fi
    {
        echo "vtr_commit=$vtr_commit"
        echo "vtr_dirty_files=$vtr_dirty"
        echo "arch=$(rel "$arch")"
        echo "arch_sha256=$(sha256sum "$arch" | cut -d' ' -f1)"
        echo "source=$(rel "$design")"
        echo "source_sha256=$(sha256sum "$design" | cut -d' ' -f1)"
        if [[ ${#include_dirs[@]} -gt 0 ]]; then
            printf 'include_dirs='
            local sep="" x
            for x in "${include_dirs[@]}"; do printf '%s%s' "$sep" "$(rel "$x")"; sep=:; done
            echo
        fi
        [[ ${#defines[@]} -eq 0 ]] || echo "defines=${defines[*]}"
        echo "tool=$t"
        echo "mem_cap_mb=$mem_mb"
        echo "status=$status"
        [[ $status == ok ]] && echo "blif_sha256=$(sha256sum "$blif" | cut -d' ' -f1)"
        echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$out_dir/$leaf.$t.prov"
    echo "run-oracle: $arch_name/$name $t $status"
    statuses+=("$status")
}

statuses=()
for t in "${tools[@]}"; do
    run_one "$t" || statuses+=(failed)  # an unexpected error inside run_one counts as failed
done
case " ${statuses[*]} " in
    *" failed "*) exit 1 ;;
    *" capped "*) exit 3 ;;
    *) exit 0 ;;
esac

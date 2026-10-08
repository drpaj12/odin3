#!/usr/bin/env bash
# run-oracle.sh — produce golden BLIFs for one design with the upstream oracles.
#
# usage: tools/run-oracle.sh [--tool parmys|odin|both] [--name NAME] [--golden DIR] DESIGN.v ARCH.xml
#
# Runs Yosys+Parmys and/or Odin II (each alone, via VTR's run_vtr_flow.py -start X -end X) and
# stores, under GOLDEN/<arch>/<NAME>/ (layout defined in the odin3-golden README), with <leaf> the
# last component of NAME and <tool> the VTR stage name (parmys | odin):
#   <leaf>.<tool>.blif   the netlist (only if the tool succeeded)
#   <leaf>.<tool>.prov   provenance: VTR commit, arch, source, sha256s, status
#   <leaf>.<tool>.log    flow-log tail and error lines (only if the tool failed)
#
# Defaults: --tool both, --name = design basename without .v (NAME may contain '/' to group
# designs, e.g. micro/bm_and), --golden = <repo>/../golden.
# Env: VTR_ROOT (default ~/odin3-ws/external/vtr-verilog-to-routing), ODIN3_WORK (scratch run
# directories, default <repo>/../work/oracle), ODIN3_ORACLE_MEM_MB (per-tool memory cap, default
# 6144; some regression designs make Odin II grow without bound, and an uncapped run exhausts the
# WSL VM and takes every session down). The cap is a cgroup limit (systemd-run --user --scope,
# MemoryMax, no swap) on the whole flow process tree. run_vtr_flow.py -limit_memory_usage is NOT
# used: VTR gates it on Path("ulimit").exists(), which is always false, so it is a silent no-op.
# Exit: 0 if every requested tool succeeded, 1 if any failed, 2 on usage error.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
vtr_root=${VTR_ROOT:-$HOME/odin3-ws/external/vtr-verilog-to-routing}
golden="$repo/../golden"
work=${ODIN3_WORK:-$repo/../work/oracle}
mem_mb=${ODIN3_ORACLE_MEM_MB:-6144}
tool=both
name=""

usage() {
    grep -m1 '^# usage:' "${BASH_SOURCE[0]}" | sed 's/^# //' >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --tool) tool=${2:?}; shift 2 ;;
        --name) name=${2:?}; shift 2 ;;
        --golden) golden=${2:?}; shift 2 ;;
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
    local status=ok
    (cd "$run" && systemd-run --user --scope --quiet -p MemoryMax="${mem_mb}M" -p MemorySwapMax=0 \
        "$flow" "$design" "$arch" -start "$t" -end "$t" -temp_dir "$run/temp" \
        >"$run/flow.log" 2>&1) || status=failed
    local blif="$run/temp/$stem.$t.blif"
    [[ $status == ok && -s $blif ]] || status=failed
    rm -f "$out_dir/$leaf.$t.blif" "$out_dir/$leaf.$t.log"
    if [[ $status == ok ]]; then
        cp "$blif" "$out_dir/$leaf.$t.blif"
    else
        {
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
        echo "tool=$t"
        echo "status=$status"
        [[ $status == ok ]] && echo "blif_sha256=$(sha256sum "$blif" | cut -d' ' -f1)"
        echo "date=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$out_dir/$leaf.$t.prov"
    echo "run-oracle: $arch_name/$name $t $status"
    [[ $status == ok ]]
}

rc=0
for t in "${tools[@]}"; do
    run_one "$t" || rc=1
done
exit $rc

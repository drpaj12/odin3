#!/usr/bin/env bash
# run-oracle.sh — produce golden BLIFs for one design with the upstream oracles.
#
# usage: tools/run-oracle.sh [--tool parmys|odin|both] [--name NAME] [--golden DIR] DESIGN.v ARCH.xml
#
# Runs Yosys+Parmys and/or Odin II (each alone, via VTR's run_vtr_flow.py -start X -end X) and
# stores, under GOLDEN/<arch>/<NAME>/:
#   <stem>.<tool>.blif   the netlist
#   <stem>.<tool>.prov   provenance: VTR commit, arch, source, hashes, status (see odin3-golden README)
# A tool that fails still gets a .prov (status=failed) and its log tail, so failures are recorded.
#
# Defaults: --tool both, --name = design basename without .v, --golden = <repo>/../golden.
# Env: VTR_ROOT (default ~/odin3-ws/external/vtr-verilog-to-routing), ODIN3_WORK (scratch run
# directories, default <repo>/../work/oracle). Exit: 0 if every requested tool succeeded, 1 if any
# failed, 2 on usage error.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
vtr_root=${VTR_ROOT:-$HOME/odin3-ws/external/vtr-verilog-to-routing}
golden="$repo/../golden"
work=${ODIN3_WORK:-$repo/../work/oracle}
tool=both
name=""

usage() {
    sed -n '3p' "${BASH_SOURCE[0]}" | sed 's/^# //' >&2
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
# shellcheck disable=SC1091
source "$vtr_root/.venv/bin/activate"

stem=$(basename "$design" .v)
name=${name:-$stem}
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
    (cd "$run" && "$flow" "$design" "$arch" -start "$t" -end "$t" -temp_dir "$run/temp" \
        >"$run/flow.log" 2>&1) || status=failed
    local blif="$run/temp/$stem.$t.blif"
    [[ $status == ok && -s $blif ]] || status=failed
    rm -f "$out_dir/$stem.$t.blif" "$out_dir/$stem.$t.log"
    if [[ $status == ok ]]; then
        cp "$blif" "$out_dir/$stem.$t.blif"
    else
        {
            tail -n 20 "$run/flow.log"
            echo "--- error lines in $t.out ---"
            grep -i -E 'error|fail' "$run/temp/$t.out" 2>/dev/null | head -n 20 || echo "(none found)"
        } >"$out_dir/$stem.$t.log"
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
    } >"$out_dir/$stem.$t.prov"
    echo "run-oracle: $arch_name/$name $t $status"
    [[ $status == ok ]]
}

rc=0
for t in "${tools[@]}"; do
    run_one "$t" || rc=1
done
exit $rc

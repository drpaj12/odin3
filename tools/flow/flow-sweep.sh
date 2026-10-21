#!/usr/bin/env bash
# flow-sweep: run odin3-flow with VPR over each given design in turn (the flow_vpr_sweep CTest).
# usage: flow-sweep.sh <odin3-flow command...> --out-base DIR design.v...
# Each design runs as `<command> --top <stem> --out DIR/<stem> design.v`; the first failure stops.
set -euo pipefail
cmd=()
while [[ $# -gt 0 && $1 != --out-base ]]; do cmd+=("$1"); shift; done
[[ ${1:-} == --out-base && $# -ge 3 ]] || { echo "usage: flow-sweep.sh CMD... --out-base DIR design.v..." >&2; exit 2; }
base=$2; shift 2
for design in "$@"; do
    stem=$(basename "$design" .v)
    "${cmd[@]}" --top "$stem" --out "$base/$stem" "$design"
done

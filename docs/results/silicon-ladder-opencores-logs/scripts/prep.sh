#!/usr/bin/env bash
# prep.sh NAME : build OUT/NAME/design.v (one merged file; the flow takes a single design file) from the cfg env.
set -uo pipefail
W=/home/jamiespa/odin3-ws/work; R=$W/opencores-runs; OUT=$W/silicon-ladder-out
N=$1; . $R/cfg/$N.env
D=$OUT/$N; mkdir -p $D
OF="${ODINFILES:-$FILES}"
: > $D/design.v
for f in $OF; do cat $f >> $D/design.v; echo >> $D/design.v; done
for d in ${INC:-}; do cp -n $d/*.v $D/ 2>/dev/null; cp -n $d/*.vh $D/ 2>/dev/null; done
# keep design.v itself
echo "prepared $D/design.v: $(wc -l < $D/design.v) lines, top=$TOP"

#!/usr/bin/env bash
# run_tail.sh NAME CLOCK : continue an existing flow dir with extract,lvs,recover, then measure + gate sim + collect
set -uo pipefail
W=$HOME/odin3-ws/work; OUT=$W/sifix3-out/ladder2; S=$W/odin3-sifix3-0
ODIN=$W/odin3-sifix3-7/build/release/odin3; T=$W/tools; N=$1; CLK=$2; D=$OUT/$N
. $W/opencores-runs/cfg/$N.env
cd $D
cp flow.time flow.time.part1
/usr/bin/time -f "maxrss_kb=%M wall=%e" -o flow.time.part2 timeout 43200 python3 $S/tools/silicon/silicon_flow.py $D/design.v --top $TOP --clock $CLK --out $D/flow --tools $T --odin3 $ODIN --steps extract,lvs,recover > flow.out2 2> flow.err2
echo "flow rc=$?" >> flow.time.part2
cat flow.out2

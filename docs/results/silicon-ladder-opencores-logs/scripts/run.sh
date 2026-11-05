#!/usr/bin/env bash
# run.sh NAME CLOCK : full silicon flow + measure + RTL/gate sims for one design. Call under tools/heavy-run.
set -uo pipefail
W=/home/jamiespa/odin3-ws/work; OUT=$W/silicon-ladder-out; S=$W/odin3-silicon-ladder
ODIN=/home/jamiespa/odin3-ws/odin3/build/rel-main2/odin3
T=$W/tools; N=$1; CLK=$2; D=$OUT/$N
. $W/opencores-runs/cfg/$N.env
rm -rf $D/flow; mkdir -p $D
cd $D
date +%s > start.epoch
/usr/bin/time -f "maxrss_kb=%M wall=%e" -o flow.time timeout ${FLOWLIM:-21600} python3 $S/tools/silicon/silicon_flow.py $D/design.v --top $TOP --clock $CLK --out $D/flow --tools $T --odin3 $ODIN > flow.out 2> flow.err
echo "flow rc=$?" >> flow.time
if [ -f flow/or/final.odb ]; then
  PDK=$T/pdk/sky130A OUT=$D/flow CLK=$CLK $T/mamba/envs/orcond/bin/openroad -no_init -exit $OUT/scripts/measure.tcl > measure.log 2>&1
fi
$OUT/scripts/sim.sh $N rtl > sim_rtl.txt 2>&1
if [ -f flow/or/final.v ]; then $OUT/scripts/sim.sh $N gate > sim_gate.txt 2>&1; fi
python3 $OUT/scripts/collect.py $N $CLK

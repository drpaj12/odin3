#!/usr/bin/env bash
# run_variant.sh NAME CLOCK TAG STEPS : rerun the flow with a step subset in OUT/NAME_TAG (diagnosis; flow+measure only)
set -uo pipefail
W=$HOME/odin3-ws/work; OUT=$W/silicon-ladder-out; S=$W/odin3-silicon-ladder
ODIN=$HOME/odin3-ws/odin3/build/rel-main2/odin3; T=$W/tools
N=$1; CLK=$2; TAG=$3; ST=$4; D=$OUT/${N}_$TAG
. $W/opencores-runs/cfg/$N.env
rm -rf $D; mkdir -p $D; cp $OUT/$N/design.v $D/; cp $OUT/$N/*.vh $D/ 2>/dev/null; cd $D
/usr/bin/time -f "maxrss_kb=%M wall=%e" -o flow.time timeout ${FLOWLIM:-21600} python3 $S/tools/silicon/silicon_flow.py $D/design.v --top $TOP --clock $CLK --out $D/flow --tools $T --odin3 $ODIN --steps $ST > flow.out 2> flow.err
echo "flow rc=$?" >> flow.time
if [ -f flow/or/final.odb ]; then PDK=$T/pdk/sky130A OUT=$D/flow CLK=$CLK $T/mamba/envs/orcond/bin/openroad -no_init -exit $OUT/scripts/measure.tcl > measure.log 2>&1; fi

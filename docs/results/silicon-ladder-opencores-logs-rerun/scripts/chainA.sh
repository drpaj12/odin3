#!/usr/bin/env bash
OUT=$HOME/odin3-ws/work/sifix3-out/ladder2
H=$HOME/odin3-ws/work/odin3-sifix3-0/tools/heavy-run
export ODIN3_HEAVY_OK=1 S=$HOME/odin3-ws/work/odin3-sifix3-0 ODIN=$HOME/odin3-ws/work/odin3-sifix3-7/build/release/odin3
while read n c; do
  echo "$(date +%T) start $n" >> $OUT/chain.log
  $H $OUT/scripts/run.sh $n $c > $OUT/$n.chain.out 2>&1
  echo "$(date +%T) end $n rc=$?" >> $OUT/chain.log
done <<L
$*
L
echo "$(date +%T) chain done" >> $OUT/chain.log

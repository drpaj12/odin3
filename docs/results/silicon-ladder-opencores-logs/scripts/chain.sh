#!/usr/bin/env bash
OUT=$HOME/odin3-ws/work/silicon-ladder-out
H=$HOME/odin3-ws/work/odin3-silicon-ladder/tools/heavy-run
export ODIN3_HEAVY_OK=1
while read n c; do
  echo "$(date +%T) start $n" >> $OUT/chain.log
  $H $OUT/scripts/run.sh $n $c > $OUT/$n.chain.out 2>&1
  echo "$(date +%T) end $n rc=$?" >> $OUT/chain.log
done <<'L'
mmuart sys_clk
simple_spi clk_i
i2c wb_clk_i
wb_lcd wb_clk_i
verilog_cordic_core clk
des clk
sha3_low clk
L
echo "$(date +%T) chain done" >> $OUT/chain.log

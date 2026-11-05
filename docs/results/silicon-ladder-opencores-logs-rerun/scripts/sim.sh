#!/usr/bin/env bash
# sim.sh NAME rtl|gate : iverilog the design's testbench against design.v (rtl) or flow/or/final.v + sky130 models (gate).
set -uo pipefail
W=$HOME/odin3-ws/work; OUT=$W/sifix3-out/ladder2; N=$1; M=$2
GS=${S:-$W/odin3-sifix3-0}/tools/silicon/gate_sim.py
PDKV=$W/tools/pdk/sky130A/libs.ref/sky130_fd_sc_hd/verilog
D=$OUT/$N; T=$OUT/tb/$N; S=$D/sim_$M; rm -rf $S; mkdir -p $S; cp $T/* $S/ 2>/dev/null
case $N in
 mmuart) TBTOP=tb_mmuart; TB="tb_mmuart.v"; PASS='PASS tb_mmuart' ;;
 simple_spi) TBTOP=tst_bench_top; TB="tst_bench_top.v wb_master_model.v spi_slave_model.v"; PASS='Testbench done' ;;
 i2c) TBTOP=tst_bench_top; TB="tst_bench_top.v wb_master_model.v i2c_slave_model.v"; PASS='Testbench done' ;;
 wb_lcd) TBTOP=tb_wb_lcd; TB="tb_wb_lcd.v"; PASS='PASS tb_wb_lcd' ;;
 verilog_cordic_core) TBTOP=tb_cordic; TB="tb_cordic.v"; PASS='PASS tb_cordic' ;;
 des) TBTOP=test; TB="des_test_po.v"; PASS='DES Test done' ;;
 sha3_low) TBTOP=test_keccak; TB="test_keccak.v"; PASS='Good!' ;;
esac
if [ $M = gate ]; then
  GP=""; [ $N = i2c ] && GP="--param ARST_LVL=0"
  cd $S
  python3 $GS $D/flow/or/final.v --pdk-verilog $PDKV --tb-top $TBTOP --tb $TB --pass "$PASS" -I $D $GP --out $S/gs > gs.out 2>&1
  echo "gate_sim rc=$?" > result.txt; cat gs.out >> result.txt
  V=$(python3 -c "import json;print(json.load(open('$S/gs/gate_sim.json'))['verdict'])" 2>/dev/null)
  case "$V" in
   pass) echo "VERDICT pass ($PASS)" >> result.txt;;
   x-pessimism) echo "VERDICT x-pessimism: X start fails; 0 start and 1 start pass" >> result.txt;;
   *) echo "VERDICT fail-or-not-run (bad=gate_sim $V)" >> result.txt;;
  esac
  cat result.txt; exit 0
fi
DUT="$D/design.v"
cd $S
iverilog -g2005 -DFUNCTIONAL -DUNIT_DELAY=#0 -I $D -I $S -s ${TBTOP:-x} -o sim.vvp $TB $DUT > iv.log 2>&1 ; echo "iverilog rc=$?" > result.txt
if [ -f sim.vvp ]; then timeout ${VVPLIM:-1800} vvp -n sim.vvp < /dev/null > sim.log 2>&1; echo "vvp rc=$?" >> result.txt; fi
BAD=$(grep -cE '^FAIL|ERROR:|Data compare error' sim.log 2>/dev/null)
if grep -q "$PASS" sim.log 2>/dev/null && [ "${BAD:-0}" = 0 ]; then echo "VERDICT pass ($PASS)" >> result.txt; else echo "VERDICT fail-or-not-run (bad=$BAD)" >> result.txt; fi
cat result.txt

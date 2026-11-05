#!/usr/bin/env bash
# diag.sh NAME CLOCK : which process/stage uses the memory (sampled ps every 2 s; Python stack dump at 20 s)
W=/home/jamiespa/odin3-ws/work; OUT=$W/silicon-ladder-out; S=$W/odin3-silicon-ladder
. $W/opencores-runs/cfg/$1.env
D=$OUT/${1}_diag; rm -rf $D; mkdir -p $D; cp $OUT/$1/design.v $D/; cd $D
( while true; do echo "== $(date +%T)"; ps -eo rss,comm,args --sort=-rss | head -4 | cut -c1-160; sleep 2; done ) > ps.log 2>&1 &
P=$!
python3 -c "
import faulthandler,sys,runpy
faulthandler.dump_traceback_later(20, repeat=True, file=open('$D/stack.log','w'))
sys.argv=['silicon_flow.py','$D/design.v','--top','$TOP','--clock','$2','--out','$D/flow','--tools','$W/tools','--odin3','/home/jamiespa/odin3-ws/odin3/build/rel-main2/odin3','--steps','read,elab,map,equiv,netlist']
runpy.run_path('$S/tools/silicon/silicon_flow.py', run_name='__main__')" > flow.out 2> flow.err
echo "rc=$?" >> flow.out
kill $P

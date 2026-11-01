#!/bin/sh
# run_vcd.sh <odin3> <python3>: for each tests/tb/micro/<n>_tb.v with a DUT (<n>.v), the VCD of
# `odin3 sim_tb --vcd` must equal iverilog's $dumpvars(0, <n>_tb) VCD signal by signal
# (tools/vcd-compare): same value at every time either file changes it. A `timescale is put in
# front of the testbench of both runs (the micros have none; iverilog defaults to 1s); the micros
# run with the unspaced, the spaced and the 100 ps forms in turn. Two more checks: a `$random` in
# a DUT port connection must not change the simulation output, and the VCD ends with the end time.
# Exit 0: at least one PASS and no FAIL. Exit 77 (ctest SKIP): iverilog or vvp is missing, or no
# micro could be compared.
set -u
odin3=$1
py=${2:-python3}
here=$(cd "$(dirname "$0")" && pwd)
tool="$here/../../tools/vcd-compare/vcd-compare"
if ! command -v iverilog >/dev/null 2>&1 || ! command -v vvp >/dev/null 2>&1; then
    echo "SKIP oracle iverilog/vvp not found"
    exit 77
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/odin3-vcd.XXXXXX")
fail=0
pass=0
mpass=0
skip=0
cd "$here/micro" || exit 2
variant=0
for tb in *_tb.v; do
    n=${tb%_tb.v}
    [ -f "$n.v" ] || continue
    variant=$((variant + 1))
    case $((variant % 3)) in
        0) ts='`timescale 1ns/1ps' ;;
        1) ts='`timescale 1 ns / 1 ps' ;;
        *) ts='`timescale 100 ps/10 ps' ;;
    esac
    { echo "$ts"; cat "$tb"; } >"$work/$n.tb.v"
    printf 'module dumper;\n  initial begin $dumpfile("%s/%s.ref.vcd"); $dumpvars(0, %s_tb); end\nendmodule\n' \
        "$work" "$n" "$n" >"$work/dumper.v"
    if ! iverilog -g2005 -s "${n}_tb" -s dumper -o "$work/$n.vvp" "$work/$n.tb.v" "$n.v" "$work/dumper.v" 2>"$work/$n.iverr" \
        || ! vvp "$work/$n.vvp" >"$work/$n.ref" 2>&1; then
        echo "SKIP $n (iverilog rejects)"; skip=$((skip + 1)); continue
    fi
    script="read_verilog $n.v; proc; opt; lower; sim_tb --out $work/$n.out --vcd $work/$n.vcd $work/$n.tb.v"
    if ! "$odin3" -p "$script" >"$work/$n.log" 2>&1; then
        echo "FAIL $n (odin3 error)"; tail -3 "$work/$n.log"; fail=1; continue
    fi
    if "$py" "$tool" "$work/$n.vcd" "$work/$n.ref.vcd" >"$work/$n.cmp" 2>&1; then
        echo "PASS $n [$ts] ($(cat "$work/$n.cmp"))"; pass=$((pass + 1)); mpass=$((mpass + 1))
    else
        echo "FAIL $n [$ts]"; head -12 "$work/$n.cmp"; fail=1
    fi
done

# A `$random` in a port connection is not evaluated by the sampler: output equal with and without.
cat >"$work/rnd.v" <<'V'
module rnd(input [31:0] a, output y);
  assign y = 1;
endmodule
V
cat >"$work/rnd_tb.v" <<'V'
module rnd_tb;
  reg [31:0] x;
  wire y;
  rnd u(.a($random), .y(y));
  initial begin
    x = $random;
    #1 $display("%d", x);
    #1 $display("%d", $random);
  end
endmodule
V
rscript="read_verilog rnd.v; proc; opt; lower; sim_tb --out"
(cd "$work" && "$odin3" -p "$rscript $work/rnd.plain $work/rnd_tb.v" >rnd.plain.log 2>&1 \
    && "$odin3" -p "$rscript $work/rnd.vcd.out --vcd $work/rnd.vcd $work/rnd_tb.v" >rnd.vcd.log 2>&1)
if [ -s "$work/rnd.plain" ] && cmp -s "$work/rnd.plain" "$work/rnd.vcd.out"; then
    echo "PASS random_not_sampled"; pass=$((pass + 1))
else
    echo "FAIL random_not_sampled (output differs with --vcd)"; fail=1
fi
cat >"$work/end_tb.v" <<'V'
module end_tb;
  reg [31:0] x;
  wire y;
  rnd u(.a(x), .y(y));
  initial begin x = 0; #1 x = 1; #3 $display("e"); end
endmodule
V
(cd "$work" && "$odin3" -p "$rscript $work/end.out --vcd $work/end.vcd $work/end_tb.v" >end.log 2>&1)
if tail -n 1 "$work/end.vcd" | grep -q '^#4$'; then
    echo "PASS final_timestamp"; pass=$((pass + 1))
else
    echo "FAIL final_timestamp (last line: $(tail -n 1 "$work/end.vcd"))"; fail=1
fi

echo "$pass passed, $skip skipped"
[ "$fail" -ne 0 ] && exit 1
[ "$mpass" -eq 0 ] && { echo "SKIP no micro could be compared"; exit 77; }
exit 0

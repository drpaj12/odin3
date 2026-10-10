#!/usr/bin/env bash
# tb.sh — writes the Icarus testbench that replays odin3-sim-vectors' input vectors on a reference
# Verilog netlist (ABC's or Yosys's dump of the same BLIF) and prints its outputs in the driver's
# line format, so the two outputs compare line by line.
#
# usage: tb.sh --flavor abc|yosys --blif IN.blif --vectors V.txt --ref REF.v --cycles N
#              [--clocks "NET..."]  > tb.v
#
#   --blif     the BLIF the reference was made from; its first model's .inputs/.outputs give the
#              bit names, in order (the driver's ports are runs of consecutive header names, IR-7b)
#   --vectors  odin3-sim-vectors output: its '#' header gives the port widths and the clock bits;
#              only the header is read
#   --cycles   the cycles the testbench runs (the caller's, never the driver's own count)
#   --clocks   the BLIF's latch clock nets: the driver's clock bits must name exactly these (so a
#              data input the simulator wrongly takes for a clock fails here, not on both sides)
#   --ref      the reference netlist; its first module is instantiated (by name, every port bit
#              connected by name)
#   --flavor   abc:   every bit is an escaped scalar port `\name `; a register clock `clock` that
#                     ABC adds (it drops the BLIF latch clocks) is driven with the clocks
#              yosys: `read_blif -wideports` made `base[k]` names bits of a vector port `base`
#
# The testbench reads the input bits of every cycle (field 2 of the driver's lines, one line per
# cycle) from the file named by +vec=FILE and, per cycle: drives them, waits, raises every clock,
# waits, lowers every clock, waits, and prints `=cycle inputs outputs` (the driver's cycle). The
# clocks are one pulled-down net, forced to 1 and released, so it is 0 from time 0 with no edge.
# Exit: 0 written, 1 the vectors and the BLIF ports or clocks disagree (message on stderr), 2 usage.
set -euo pipefail

usage() {
    echo "usage: tb.sh --flavor abc|yosys --blif IN.blif --vectors V.txt --ref REF.v --cycles N [--clocks \"NET...\"]" >&2
    exit 2
}

# Prints "in NAME" / "out NAME" for each header name of the first model, in order.
blif_ports() {
    LC_ALL=C awk '
    { line = $0; sub(/#.*/, "", line) }
    acc != "" || line ~ /\\[ \t\r]*$/ {
        cont = sub(/\\[ \t\r]*$/, "", line)
        acc = (acc == "" ? line : acc " " line)
        if (cont) next
        line = acc; acc = ""
    }
    {
        n = split(line, t, /[ \t\r]+/)
        k = 1; while (k <= n && t[k] == "") k++
        if (t[k] == ".end") exit
        if (t[k] == ".inputs" || t[k] == ".outputs")
            for (i = k + 1; i <= n; i++) if (t[i] != "") print (t[k] == ".inputs" ? "in" : "out"), t[i]
    }' "$1"
}

main() {
    local flavor="" blif="" vectors="" ref="" cycles="" clocks="" check_clocks=0
    while [ $# -gt 0 ]; do
        case $1 in
        --flavor) flavor=${2:-}; shift 2 || usage ;;
        --blif) blif=${2:-}; shift 2 || usage ;;
        --vectors) vectors=${2:-}; shift 2 || usage ;;
        --ref) ref=${2:-}; shift 2 || usage ;;
        --cycles) cycles=${2:-}; shift 2 || usage ;;
        --clocks) clocks=${2-}; check_clocks=1; shift 2 || usage ;;
        *) usage ;;
        esac
    done
    case $flavor in abc | yosys) ;; *) usage ;; esac
    [ -n "$blif" ] && [ -n "$vectors" ] && [ -n "$ref" ] || usage
    case $cycles in '' | *[!0-9]*) usage ;; esac
    local module abc_clock=0
    module=$(LC_ALL=C awk '/^module / {
            m = substr($0, 8)
            if (substr(m, 1, 1) == "\\") { sub(/ .*/, "", m); m = m " " } else sub(/[ (;].*/, "", m)
            print m; exit }' "$ref")
    [ -n "$module" ] || { echo "tb.sh: no module in $ref" >&2; exit 1; }
    if [ "$flavor" = abc ] && grep -q '^ *always @ (posedge clock)' "$ref"; then abc_clock=1; fi
    grep '^#' "$vectors" | FLAVOR=$flavor MODULE=$module ABC_CLOCK=$abc_clock CYCLES=$cycles \
        CLOCKS=$clocks CHECK_CLOCKS=$check_clocks LC_ALL=C awk '
    function esc(name) { return "\\" name " " }
    # Yosys wideports_split (frontends/blif/blifparse.cc): sets wbase/widx when name is base[int].
    function wsplit(name,   len, i, pos, c, nx) {
        len = length(name); pos = -1
        if (len == 0 || substr(name, len, 1) != "]") return 0
        for (i = 0; i + 1 < len; i++) {
            c = substr(name, i + 1, 1); nx = substr(name, i + 2, 1)
            if (c == "[") pos = i
            else if (c != "-" && (c < "0" || c > "9")) pos = -1
            else if (c == "-" && (i != pos + 1 || nx == "]")) pos = -1
            else if (i == pos + 2 && c == "0" && substr(name, i, 1) == "-") pos = -1
            else if (i == pos + 1 && c == "0" && nx != "]") pos = -1
        }
        if (pos < 0) return 0
        wbase = substr(name, 1, pos); widx = substr(name, pos + 2, len - pos - 2) + 0
        return widx >= 0
    }
    function fail(msg) { print "tb.sh: " msg > "/dev/stderr"; bad = 1; exit 1 }
    FNR == NR {
        if (FNR == 1) { ncyc = ENVIRON["CYCLES"] + 0; next }
        if ($2 == "input" || $2 == "clock") {
            if ($3 != lastin) { nin++; inname[nin] = $3; lastin = $3 }
            if ($2 == "input") inw[nin] += $4; else { inw[nin]++; isclk[nin, $4] = 1 }
        } else if ($2 == "output") { nout++; outname[nout] = $3; outw[nout] = $4 }
        next
    }
    $1 == "in" { bin[++nbin] = $2; next }
    $1 == "out" { bout[++nbout] = $2; next }
    END {
        if (bad) exit 1
        # Input bit k of port p is header name at+k; the driver prints k = W-1 .. 0, clocks left out.
        at = 0; wi = 0
        for (p = 1; p <= nin; p++) at += inw[p]
        if (at != nbin) fail("vectors have " at " input bits, the BLIF top declares " nbin " input ports")
        at = 0
        for (p = 1; p <= nin; p++) {
            for (k = inw[p] - 1; k >= 0; k--) if (!((p, k) in isclk)) { order[++wi] = at + k + 1 }
            for (k = 0; k < inw[p]; k++) if ((p, k) in isclk) sig[at + k + 1] = "tb_clk"
            at += inw[p]
        }
        for (j = 1; j <= wi; j++) sig[order[j]] = "tb_in[" (wi - j) "]"
        if (ENVIRON["CHECK_CLOCKS"] == "1") {
            nw = split(ENVIRON["CLOCKS"], want, " "); have = ""; miss = ""; nh = 0
            for (k = 1; k <= nw; k++) wanted[want[k]] = 1
            for (j = 1; j <= nbin; j++) if (sig[j] == "tb_clk") {
                nh++; got[bin[j]] = 1
                if (!(bin[j] in wanted)) have = have " " bin[j]
            }
            for (k = 1; k <= nw; k++) if (!(want[k] in got)) miss = miss " " want[k]
            if (have != "" || miss != "")
                fail("clock bits differ from the BLIF latch clocks (simulator only:" have "; BLIF only:" miss ")")
        }
        at = 0; wo = 0
        for (p = 1; p <= nout; p++) at += outw[p]
        if (at != nbout) fail("vectors have " at " output bits, the BLIF top declares " nbout " output ports")
        at = 0
        # Output bit k of port p prints at column at+W-1-k; %b prints tb_out MSB first.
        for (p = 1; p <= nout; p++) {
            for (k = 0; k < outw[p]; k++) osig[at + k + 1] = "tb_out[" (nbout - 1 - (at + outw[p] - 1 - k)) "]"
            at += outw[p]
        }
        ncon = 0
        if (ENVIRON["FLAVOR"] == "abc") {
            for (j = 1; j <= nbin; j++) con[++ncon] = "." esc(bin[j]) "(" sig[j] ")"
            for (j = 1; j <= nbout; j++) con[++ncon] = "." esc(bout[j]) "(" osig[j] ")"
            if (ENVIRON["ABC_CLOCK"] == "1") {
                for (j = 1; j <= nbin; j++) if (bin[j] == "clock" && sig[j] != "tb_clk")
                    fail("ABC clocks its registers by input clock, which is not a clock here")
                if (!seenclock()) con[++ncon] = ".clock(tb_clk)"
            }
        } else {
            nc = 0
            for (j = 1; j <= nbin; j++) wide(bin[j], sig[j], "in")
            for (j = 1; j <= nbout; j++) wide(bout[j], osig[j], "out")
            for (b = 1; b <= nbase; b++) {
                name = bases[b]; w = bw[name]; s = ""
                for (k = w - 1; k >= 0; k--) {
                    v = ((name, k) in bsig) ? bsig[name, k] : (bdir[name] == "in" ? "1'\''b0" : "tb_nc[" nc++ "]")
                    s = s (s == "" ? "" : ", ") v
                }
                con[++ncon] = "." esc(name) "(" (w > 1 ? "{" s "}" : s) ")"
            }
        }
        emit()
    }
    function seenclock(   j) { for (j = 1; j <= nbin; j++) if (bin[j] == "clock") return 1; return 0 }
    function wide(name, s, dir) {
        if (!wsplit(name)) { con[++ncon] = "." esc(name) "(" s ")"; return }
        if (!(wbase in bw)) { bases[++nbase] = wbase; bw[wbase] = 0; bdir[wbase] = dir }
        if (bdir[wbase] != dir) fail("vector " wbase " has both input and output bits")
        if (widx + 1 > bw[wbase]) bw[wbase] = widx + 1
        bsig[wbase, widx] = s
    }
    function emit(   j) {
        print "// Generated by tools/sim-check/tb.sh; replays odin3-sim-vectors inputs."
        print "`timescale 1ns/1ns"
        print "module sim_check_tb;"
        if (wi > 0) { print "  reg [" wi - 1 ":0] tb_mem [0:" (ncyc > 0 ? ncyc - 1 : 0) "];"; print "  reg [" wi - 1 ":0] tb_in;" }
        if (nbout > 0) print "  wire [" nbout - 1 ":0] tb_out;"
        if (nc > 0) print "  wire [" nc - 1 ":0] tb_nc;"
        # A pulled-down net starts at 0 with no event: a reg would step x -> 0 at time 0, a
        # negedge that clocks falling-edge registers before the first cycle.
        print "  tri0 tb_clk;"
        print "  reg [8*4096-1:0] tb_path;"
        print "  integer tb_cycle;"
        printf "  %s tb_dut (", ENVIRON["MODULE"]
        for (j = 1; j <= ncon; j++) printf "%s\n    %s", (j > 1 ? "," : ""), con[j]
        print ");"
        print "  initial begin"
        if (wi > 0) {
            print "    if (!$value$plusargs(\"vec=%s\", tb_path)) begin"
            print "      $display(\"sim_check_tb: no +vec=FILE\");"
            print "      $finish;"
            print "    end"
            print "    $readmemb(tb_path, tb_mem);"
        }
        print "    for (tb_cycle = 0; tb_cycle < " ncyc "; tb_cycle = tb_cycle + 1) begin"
        if (wi > 0) print "      tb_in = tb_mem[tb_cycle];"
        print "      #1 force tb_clk = 1'\''b1;"
        print "      #1 release tb_clk;"
        printf "      #1 $display(\"=%%0d %s %s\", tb_cycle%s%s);\n", (wi > 0 ? "%b" : "-"), (nbout > 0 ? "%b" : "-"), (wi > 0 ? ", tb_in" : ""), (nbout > 0 ? ", tb_out" : "")
        print "    end"
        print "    $finish;"
        print "  end"
        print "endmodule"
    }' - <(blif_ports "$blif")
}

main "$@"

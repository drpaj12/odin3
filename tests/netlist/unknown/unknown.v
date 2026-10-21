// unknown: a netlist that instantiates a cell no library defines (spec G-7). Without a library
// read_verilog says unresolved_module at the instance; with --auto-blackbox the cell becomes an
// implicit black box (Q drives the otherwise undriven net w, so it is an output).
module unknown (output y);
    wire w;
    MYSTERY u1 (.Q(w));
    assign y = w;
endmodule

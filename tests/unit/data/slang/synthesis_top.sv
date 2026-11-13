// SYNTHESIS is predefined (as Yosys does), so the ifndef branch is skipped and the error never fires.
`ifndef SYNTHESIS
  `error "SYNTHESIS not predefined"
`endif
module synthesis_top (
    input  logic a,
    output wire  y
);
    assign y = a;
endmodule

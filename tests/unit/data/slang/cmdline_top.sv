// Read with the command's -D and -I: WIDTH comes from project/inc/defs.svh (-I), CMD_DEF from -D.
`include "defs.svh"
`ifndef CMD_DEF
  `error "CMD_DEF not defined"
`endif
module cmdline_top (
    input  logic [`WIDTH-1:0] a,
    output wire  [`WIDTH-1:0] y
);
    assign y = a;
endmodule

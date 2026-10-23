// The project's incdir makes this include resolve; WIDTH comes from inc/defs.svh.
`include "defs.svh"
module inc_top (
    input  logic [`WIDTH-1:0] a,
    output wire  [`WIDTH-1:0] y
);
    assign y = a;
endmodule

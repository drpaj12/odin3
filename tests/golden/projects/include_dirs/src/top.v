// `include search: widths.vh is found on the include path (src/include),
// ops.vh next to this file (the including file's directory is searched first).
`include "widths.vh"
`include "ops.vh"

module top (
    input  wire [`W-1:0] a,
    input  wire [`W-1:0] b,
    output wire [`W-1:0] y
);
    assign y = `OP(a, b);
endmodule

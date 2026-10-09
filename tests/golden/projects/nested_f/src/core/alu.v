// a + b, width from the include path set in the outer file list.
`include "nf_width.vh"

module alu (
    input  wire [`W-1:0] a,
    input  wire [`W-1:0] b,
    output wire [`W-1:0] y
);
    assign y = a + b;
endmodule

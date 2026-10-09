// Two global macros: USE_SUB (no value) picks the operation, WIDTH (valued) the width.
module top (
    input  wire [`WIDTH-1:0] a,
    input  wire [`WIDTH-1:0] b,
    output wire [`WIDTH-1:0] y
);
`ifdef USE_SUB
    assign y = a - b;
`else
    assign y = a + b;
`endif
endmodule

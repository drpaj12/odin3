// Global macros: USE_SUB (no value) picks the operation, WIDTH (valued) the width,
// TAG (a string macro, quotes included in its value) a constant output.
module top (
    input  wire [`WIDTH-1:0] a,
    input  wire [`WIDTH-1:0] b,
    output wire [`WIDTH-1:0] y,
    output wire [15:0]       tag
);
`ifdef USE_SUB
    assign y = a - b;
`else
    assign y = a + b;
`endif
    assign tag = `TAG;
endmodule

// Bitwise majority of three 2-bit inputs.
module leaf (
    input  wire [1:0] a,
    input  wire [1:0] b,
    input  wire [1:0] c,
    output wire [1:0] y
);
    assign y = (a & b) | (a & c) | (b & c);
endmodule

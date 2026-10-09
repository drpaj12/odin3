// Saturating 4-bit add, found by library search.
module sat4 (
    input  wire [3:0] a,
    input  wire [3:0] b,
    output wire [3:0] y
);
    wire [4:0] s = a + b;

    assign y = s[4] ? `SAT_MAX : s[3:0];
endmodule

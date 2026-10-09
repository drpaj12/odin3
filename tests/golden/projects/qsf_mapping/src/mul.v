// Unsigned W x W multiplier.
module mul #(parameter W = 8) (
    input  wire [W-1:0]   a,
    input  wire [W-1:0]   b,
    output wire [2*W-1:0] p
);
    assign p = a * b;
endmodule

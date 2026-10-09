// W-bit adder with carry out.
module adder #(parameter W = 8) (
    input  wire [W-1:0] a,
    input  wire [W-1:0] b,
    output wire [W:0]   s
);
    assign s = a + b;
endmodule

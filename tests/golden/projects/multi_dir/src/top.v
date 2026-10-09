// Registers a + b (with carry) when sel is 1, else a zero-extended.
module top (
    input  wire       clk,
    input  wire       sel,
    input  wire [3:0] a,
    input  wire [3:0] b,
    output wire [4:0] y
);
    wire [4:0] sum;

    adder #(.W(4)) u_add (.a(a), .b(b), .s(sum));
    pick u_pick (.clk(clk), .sel(sel), .d0({1'b0, a}), .d1(sum), .q(y));
endmodule

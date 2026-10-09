// Flip-flop with synchronous reset.
module dffr (
    input  wire clk,
    input  wire rst,
    input  wire d,
    output reg  q
);
    always @(posedge clk)
        q <= rst ? 1'b0 : d;
endmodule

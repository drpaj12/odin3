// One pipeline register.
module stage_reg #(parameter W = 1) (
    input  wire         clk,
    input  wire [W-1:0] d,
    output reg  [W-1:0] q
);
    always @(posedge clk)
        q <= d;
endmodule

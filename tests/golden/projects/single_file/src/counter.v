// 4-bit counter with enable and synchronous clear.
module counter (
    input  wire       clk,
    input  wire       en,
    input  wire       clr,
    output reg  [3:0] q
);
    always @(posedge clk)
        if (clr)
            q <= 4'd0;
        else if (en)
            q <= q + 4'd1;
endmodule

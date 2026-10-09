// W resettable flip-flops.
`include "nf_width.vh"

module regs (
    input  wire          clk,
    input  wire          rst,
    input  wire [`W-1:0] d,
    output wire [`W-1:0] q
);
    genvar i;
    generate
        for (i = 0; i < `W; i = i + 1) begin : g_bit
            dffr u_ff (.clk(clk), .rst(rst), .d(d[i]), .q(q[i]));
        end
    endgenerate
endmodule

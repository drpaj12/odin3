// Uses an "IP core" whose files come from ip/fifo/fifo.qip.
module top (
    input  wire       clk,
    input  wire       we,
    input  wire [3:0] d,
    output wire [3:0] q
);
    fifo2 u_fifo (.clk(clk), .we(we), .d(d), .q(q));
endmodule

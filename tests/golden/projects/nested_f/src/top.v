// Registered ALU; its files come from two nested file lists.
`include "nf_width.vh"

module top (
    input  wire          clk,
    input  wire          rst,
    input  wire [`W-1:0] a,
    input  wire [`W-1:0] b,
    output wire [`W-1:0] q
);
    wire [`W-1:0] r;

    alu u_alu (.a(a), .b(b), .y(r));
    regs u_regs (.clk(clk), .rst(rst), .d(r), .q(q));
endmodule

// Uses and2 and mux2 (found in the -y directory lib/cells) and dff (in the -v file).
module top (
    input  wire clk,
    input  wire a,
    input  wire b,
    input  wire s,
    output wire q
);
    wire n, m;

    and2 u_and (.a(a), .b(b), .y(n));
    mux2 u_mux (.a(a), .b(n), .s(s), .y(m));
    dff u_dff (.clk(clk), .d(m), .q(q));
endmodule

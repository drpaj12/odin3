// Two multipliers; the project keeps the small one in soft logic.
module top (
    input  wire [7:0]  a,
    input  wire [7:0]  b,
    input  wire [3:0]  c,
    input  wire [3:0]  d,
    output wire [15:0] p,
    output wire [7:0]  r
);
    mul #(.W(8)) u_big (.a(a), .b(b), .p(p));
    mul #(.W(4)) u_small (.a(c), .b(d), .p(r));
endmodule

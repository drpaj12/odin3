// 4.2 B-4: a connection wider than the port is truncated, with the elaboration warning.
module generic_wide (
    input [9:0] a,
    input [7:0] b,
    output [15:0] prod
);
  mult_8 u_mul (.a(a), .b(b), .out(prod));
endmodule

// 4.2 B-3: a port the model does not have is the located elaboration error.
module generic_unknown_port (
    input [7:0] a,
    input [7:0] b,
    output [15:0] prod
);
  mult_8 u_mul (.a(a), .b(b), .out(prod), .foo(a));
endmodule

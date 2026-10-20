// 4.2 B-5 (review M1): every bit of port 'b' is an explicit x; the IR cannot tell it from an
// unconnected port, so it is tied to gnd and the warning says "not connected or is x".
module generic_explicit_x (
    input [7:0] a,
    output [15:0] prod
);
  mult_8 u_mul (.a(a), .b(8'bx), .out(prod));
endmodule

// 4.2 B-5 (review M5): one bit of port 'a' is x, the others are driven: not "not connected", so
// nothing is tied to gnd.
module generic_partial_x (
    input [7:0] a,
    input [7:0] b,
    output [15:0] prod
);
  mult_8 u_mul (.a({a[7:1], 1'bx}), .b(b), .out(prod));
endmodule

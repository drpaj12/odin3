// 4.2 B-5: input port 'b' is not connected; partial_map ties it to gnd with a warning.
module generic_missing_input (
    input [7:0] a,
    output [15:0] prod
);
  mult_8 u_mul (.a(a), .out(prod));
endmodule

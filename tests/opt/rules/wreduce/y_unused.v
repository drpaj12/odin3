// only the low 4 bits of the 8-bit result are read
module y_unused(input [7:0] a, input [7:0] b, output [3:0] y);
  wire [7:0] t = a & b;
  assign y = t[3:0];
endmodule

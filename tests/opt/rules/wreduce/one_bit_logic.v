// $logic_and (and a comparison) with a wide Y
module one_bit_logic(input [7:0] a, input [7:0] b, output [7:0] y, output [7:0] z);
  assign y = a && b;
  assign z = a < b;
endmodule

// $sub pads with its top Y bit, signed or not
module y_cap_sub(input [7:0] a, input [7:0] b, output [15:0] y);
  assign y = a - b;
endmodule

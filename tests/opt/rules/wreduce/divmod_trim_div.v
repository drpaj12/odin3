// $div: the zero top bits of an unsigned A go (pre-step, A then B)
module divmod_trim_div(input [4:0] a, input [7:0] b, output [7:0] y);
  assign y = {3'b000, a} / b;
endmodule

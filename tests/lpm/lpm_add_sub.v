// 5.1 micro: an adder and a subtractor, signed and unsigned (maps to lpm_add_sub).
module lpm_add_sub_micro(input [7:0] a, input [7:0] b, input signed [5:0] sa, input signed [5:0] sb,
                         output [8:0] s, output [8:0] d, output signed [6:0] ss, output signed [6:0] sd);
  assign s = a + b;
  assign d = a - b;
  assign ss = sa + sb;
  assign sd = sa - sb;
endmodule

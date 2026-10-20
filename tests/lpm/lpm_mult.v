// 5.1 micro: an unsigned and a signed multiplier (maps to lpm_mult).
module lpm_mult_micro(input [7:0] a, input [6:0] b, input signed [5:0] sa, input signed [4:0] sb,
                      output [14:0] p, output signed [10:0] sp);
  assign p = a * b;
  assign sp = sa * sb;
endmodule

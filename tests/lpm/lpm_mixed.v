// 5.1 micro: mixed widths and mixed signs (spec A-2, M-2, C-2): an add of an 8 and a 4 bit
// operand, a signed 4 + 8 bit add, a mixed-sign compare (unsigned), a compare of different
// widths, and a multiplier whose Y is shorter than the product (the low bits).
module lpm_mixed_micro(input [7:0] a, input [3:0] b, input signed [3:0] sa, input signed [7:0] sb,
                       output [8:0] s_unsigned, output signed [8:0] s_signed,
                       output lt_mixed, output ge_widths, output [9:0] p_short);
  assign s_unsigned = a + b;
  assign s_signed = sa + sb;
  assign lt_mixed = a < sb;
  assign ge_widths = a >= b;
  assign p_short = a * b;
endmodule

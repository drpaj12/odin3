// 5.1 micro: the six comparisons, unsigned and signed (map to lpm_compare).
module lpm_compare_micro(input [7:0] a, input [7:0] b, input signed [5:0] sa, input signed [5:0] sb,
                         output lt, output le, output gt, output ge, output eq, output ne,
                         output slt, output sge);
  assign lt = a < b;
  assign le = a <= b;
  assign gt = a > b;
  assign ge = a >= b;
  assign eq = a == b;
  assign ne = a != b;
  assign slt = sa < sb;
  assign sge = sa >= sb;
endmodule

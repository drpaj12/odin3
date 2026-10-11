// T3: incomplete sensitivity list on combinational code — variants: @(a) reading a and b, @(a or c) reading a, b and c
module t3_incomplete (input a, b, c, output reg y, z);
  always @(a)
    y = a & b;
  always @(a or c)
    z = a | b | c;
endmodule

// E18: $signed() and $unsigned() — variants: $signed(a) alone, $signed(a) + $signed(b), $unsigned(s) >> 1
module e18_signed_calls (input [3:0] a, b, input signed [3:0] s, output signed [3:0] y1, output signed [4:0] y2, output [3:0] y3);
  assign y1 = $signed(a);
  assign y2 = $signed(a) + $signed(b);
  assign y3 = $unsigned(s) >> 1;
endmodule

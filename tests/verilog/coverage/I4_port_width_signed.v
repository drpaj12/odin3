// I4: port width mismatch and signed ports — variants: a narrower and a wider net on ports (extend or truncate), a signed port connected to an unsigned net
module i4_leaf (input signed [3:0] a, output signed [7:0] y);
  assign y = a;
endmodule

module i4_widths (input [1:0] narrow, input [7:0] wide, output [11:0] y1, output [3:0] y2);
  i4_leaf u1 (.a(narrow), .y(y1));
  i4_leaf u2 (.a(wide), .y(y2));
endmodule

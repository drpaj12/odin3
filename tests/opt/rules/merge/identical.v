// merge/identical: two identical $add cells -> one
module t(input [3:0] a, input [3:0] b, output [3:0] y, output [3:0] z);
  assign y = a + b;
  assign z = a + b;
endmodule

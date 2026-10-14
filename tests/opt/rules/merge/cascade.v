// merge/cascade: merging two $add cells makes two $mul cells identical
module t(input [3:0] a, input [3:0] b, input [3:0] c, output [3:0] y, output [3:0] z);
  assign y = (a + b) * c;
  assign z = (a + b) * c;
endmodule

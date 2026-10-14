// expr/invert.double: ~~a -> a
module t(input a, output y);
  assign y = ~(~a);
endmodule

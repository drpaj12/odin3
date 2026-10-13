// expr/fine.reduce_and: -fine: $reduce_and of a constant x vector -> one-bit constant
// options: -fine
module t(input a, output y);
  wire [1:0] c = 2'bxx;
  assign y = &c;
endmodule

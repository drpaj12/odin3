// expr/fine.neutral_A: -fine: $reduce_or drops its constant-0 input bits
// options: -fine
module t(input [1:0] a, output y);
  assign y = |{a, 2'b00};
endmodule

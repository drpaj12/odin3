// reduce/or_const1: $reduce_or with a constant-1 input -> that input alone
module t(input a, input b, output y);
  assign y = |{a, b, 1'b1};
endmodule

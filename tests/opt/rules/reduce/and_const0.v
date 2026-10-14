// reduce/and_const0: $reduce_and with a constant-0 input -> that input alone
module t(input a, input b, output y);
  assign y = &{a, b, 1'b0};
endmodule

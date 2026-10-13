// reduce/and_neutral: $reduce_and with a constant-1 input -> dropped
module t(input a, input b, output y);
  assign y = &{a, b, 1'b1};
endmodule

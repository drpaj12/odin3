// reduce/or_neutral: $reduce_or with a constant-0 input -> dropped
module t(input a, input b, output y);
  assign y = |{a, b, 1'b0};
endmodule

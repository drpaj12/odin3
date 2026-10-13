// reduce/or_dup: $reduce_or with a repeated input -> deduplicated
module t(input a, input b, output y);
  assign y = |{a, b, a};
endmodule

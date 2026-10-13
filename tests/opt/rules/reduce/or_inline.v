// reduce/or_inline: $reduce_or of a $reduce_or -> the child's inputs inlined
module t(input a, input b, input c, output y);
  wire t = |{a, b};
  assign y = |{t, c};
endmodule

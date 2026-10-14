// reduce/and_inline: $reduce_and of a $reduce_and -> the child's inputs inlined
module t(input a, input b, input c, output y);
  wire t = &{a, b};
  assign y = &{t, c};
endmodule

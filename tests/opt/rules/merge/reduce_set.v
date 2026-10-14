// merge/reduce_set: $reduce_and over the same set of inputs in another order -> one
module t(input a, input b, input c, output y, output z);
  assign y = &{a, b, c};
  assign z = &{c, a, b};
endmodule

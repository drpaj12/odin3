// merge/reduce_multiset: $reduce_xor over the same multiset in another order -> one
module t(input a, input b, input c, output y, output z);
  assign y = ^{a, b, c};
  assign z = ^{c, b, a};
endmodule

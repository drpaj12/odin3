// expr/reduce_xnor_not: 1-bit $reduce_xnor becomes a $not
module t(input a, output y);
  assign y = ~^a;
endmodule

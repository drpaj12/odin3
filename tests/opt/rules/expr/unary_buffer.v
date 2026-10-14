// expr/unary_buffer: 1-bit $reduce_xor with a 1-bit result -> Y = A
module t(input a, output y);
  assign y = ^a;
endmodule

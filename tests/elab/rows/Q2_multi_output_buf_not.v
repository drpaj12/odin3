// Q2: buf and not with several outputs — variants: buf with three outputs, not with two outputs
module q2_multi_output (input a, output y1, y2, y3, n1, n2);
  buf b1 (y1, y2, y3, a);
  not n (n1, n2, a);
endmodule

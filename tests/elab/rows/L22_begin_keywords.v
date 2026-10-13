// L22: `begin_keywords / `end_keywords — variants: "1364-1995", "1364-2001", "1364-2001-noconfig", "1364-2005", a set nested inside another (the unknown-version error is a tests/verilog/reject case)
`begin_keywords "1364-1995"
module l22_1995 (a, y);
  input a;
  output y;
  wire generate;
  assign generate = a;
  assign y = generate;
endmodule
`begin_keywords "1364-2001"
module l22_2001 (input signed [1:0] a, output uwire);
  assign uwire = a;
endmodule
`end_keywords
`end_keywords
`begin_keywords "1364-2001-noconfig"
module l22_noconfig (input a, output config);
  assign config = a;
endmodule
`end_keywords
`begin_keywords "1364-2005"
module l22_2005 (input a, output y);
  uwire w;
  assign w = a;
  assign y = w;
endmodule
`end_keywords
module l22_top (input a, input [1:0] b, output y1, y2, y3, y4);
  wire unused;
  l22_1995 u1 (.a(a), .y(y1));
  l22_2001 u2 (b, unused);
  l22_noconfig u3 (a, y3);
  l22_2005 u4 (.a(a), .y(y4));
  assign y2 = unused;
endmodule

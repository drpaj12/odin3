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

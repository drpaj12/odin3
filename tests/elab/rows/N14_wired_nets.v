// N14: wired nets — variants: wand, wor, triand and trior, each with two continuous drivers
module n14_wired_nets (input a, b, output y1, y2, y3, y4);
  wand wa;
  wor wo;
  triand ta;
  trior to;
  assign wa = a;
  assign wa = b;
  assign wo = a;
  assign wo = b;
  assign ta = a;
  assign ta = b;
  assign to = a;
  assign to = b;
  assign y1 = wa;
  assign y2 = wo;
  assign y3 = ta;
  assign y4 = to;
endmodule

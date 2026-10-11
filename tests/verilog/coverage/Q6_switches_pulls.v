// Q6: MOS, CMOS and pass switches, pullup and pulldown (REJECT) — variants: nmos pmos rnmos rpmos cmos rcmos, tran rtran tranif0 tranif1 rtranif0 rtranif1, pullup with a strength, pulldown
module q6_switches (input d, c, nc, inout io1, io2, output y1, y2, y3, y4, y5, y6, p, n);
  nmos m1 (y1, d, c);
  pmos m2 (y2, d, c);
  rnmos m3 (y3, d, c);
  rpmos m4 (y4, d, c);
  cmos m5 (y5, d, c, nc);
  rcmos m6 (y6, d, c, nc);
  tran t1 (io1, io2);
  rtran t2 (io1, io2);
  tranif0 t3 (io1, io2, c);
  tranif1 t4 (io1, io2, c);
  rtranif0 t5 (io1, io2, c);
  rtranif1 t6 (io1, io2, c);
  pullup (strong1) (p);
  pulldown (n);
endmodule

// N15: uwire, tri0, tri1, trireg — variants: uwire (SHOULD), uwire with a range, tri0, tri1, trireg (REJECT)
module n15_uwire_tri (input a, output y);
  uwire u;
  uwire [3:0] uv;
  tri0 t0;
  tri1 t1;
  trireg tr;
  assign u = a;
  assign y = u;
endmodule

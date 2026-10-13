// E17: out-of-range and x/z index reads — variants: a constant index past the range a[9], an x index a[4'bx], a z index a[4'bz], a variable index wider than the range a[s]
module e17_out_of_range (input [7:0] a, input [3:0] s, output y1, y2, y3, y4);
  assign y1 = a[9];
  assign y2 = a[4'bx];
  assign y3 = a[4'bz];
  assign y4 = a[s];
endmodule

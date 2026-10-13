// D3 (accepted, 2D-7b): a tri-state output of a child module (not a top-level port), and a bus
// made of two disjoint halves with opposite enables, read through logic.
module d3_drv (input [3:0] d, input en, output [3:0] q);
  assign q = en ? d : 4'bz;
endmodule
module d3_top (input [3:0] a, b, input s, output [3:0] y, output [3:0] z);
  wire [3:0] w;
  d3_drv u0 (.d(a), .en(s), .q(w));
  assign y = w + 4'd1;
  wire [3:0] h;
  assign h[1:0] = s ? a[1:0] : 2'bz;
  assign h[3:2] = s ? 2'bz : b[3:2];
  assign z = ~h;
endmodule

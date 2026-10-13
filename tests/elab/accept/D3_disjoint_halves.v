// D3 (accepted, 2D-7b): a bus made of two disjoint halves with opposite enables, read through logic.
// (A tri-state output of a module below the top is a located error: the instance may drive a
// top-level port, where Yosys keeps the $tribuf; see test_tribuf_below_top.)
module d3_top (input [3:0] a, b, input s, output [3:0] z);
  wire [3:0] h;
  assign h[1:0] = s ? a[1:0] : 2'bz;
  assign h[3:2] = s ? 2'bz : b[3:2];
  assign z = ~h;
endmodule

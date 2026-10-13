// PR 2D-6 re-review N1: two blocks write disjoint halves of w, each under a runtime if/else.
module t(input [7:0] a, input [7:0] b, output [7:0] q);
  reg [7:0] w;
  assign q = w;
  always @* begin : k1
    integer i;
    for (i = 0; i < 4; i = i + 1) if (a[i]) w[i] = b[i]; else w[i] = 0;
  end
  always @* begin : k2
    integer i;
    for (i = 0; i < 4; i = i + 1) if (a[i+4]) w[4+i] = b[i]; else w[4+i] = 1;
  end
endmodule

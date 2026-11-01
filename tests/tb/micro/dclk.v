// Derived clocks: a divided clock (a register's output), a divider of a divider, a gated clock
// (the input clock and a register) and a mux of the input clock and a divided one. Every register
// starts at 0 (no x) so iverilog and Odin III agree from the first cycle.
module dclk(input clk, input en_in, input sel, input [7:0] d, output reg [7:0] a, output reg [7:0] b,
            output reg [7:0] c, output reg [7:0] e, output reg div, output reg div4);
  reg en_q;
  initial begin en_q = 0; div = 0; div4 = 0; a = 0; b = 0; c = 0; e = 0; end
  always @(negedge clk) en_q <= en_in;
  always @(posedge clk) div <= ~div;
  always @(posedge div) div4 <= ~div4;
  wire gclk = clk & en_q;
  wire mclk = sel ? div : clk;
  always @(posedge div) a <= a + d;
  always @(posedge gclk) b <= b + d + 8'd1;
  always @(posedge mclk) c <= c + d;
  always @(posedge div4) e <= e + 8'd1;
endmodule

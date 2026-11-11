// A dual-port RAM on two clocks: written on wclk, read on rclk (a registered read) and read
// asynchronously (aq).
module mem2clk(input wclk, input rclk, input we, input [2:0] wa, input [7:0] wd,
               input [2:0] ra, output reg [7:0] rq, output [7:0] aq);
  reg [7:0] m [0:7];
  always @(posedge wclk) if (we) m[wa] <= wd;
  always @(posedge rclk) rq <= m[ra];
  assign aq = m[ra];
endmodule

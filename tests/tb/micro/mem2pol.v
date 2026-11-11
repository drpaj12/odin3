// Mixed polarity: written on the rising edge of wclk, read on the falling edge of rclk.
module mem2pol(input wclk, input rclk, input we, input [2:0] wa, input [7:0] wd,
               input [2:0] ra, output reg [7:0] rq, output [7:0] aq);
  reg [7:0] m [0:7];
  always @(posedge wclk) if (we) m[wa] <= wd;
  always @(negedge rclk) rq <= m[ra];
  assign aq = m[ra];
endmodule

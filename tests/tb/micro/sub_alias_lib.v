module counter(input clk, output reg [3:0] cnt);
  initial cnt = 0;
  always @(posedge clk) cnt <= cnt + 1;
endmodule

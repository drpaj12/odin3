module oldstyle(clk, d, q, busy);
  input clk;
  input [7:0] d;
  output [7:0] q;
  output busy;
  reg [7:0] q;
  reg busy;
  integer n;
  initial begin n = 0; busy = 0; end
  always @(posedge clk) begin
    q <= d;
    n = n + 1;
    busy <= (n > 2);
  end
endmodule

module ansistyle(input wire clk, input rst, input [3:0] wide, output reg par, output [1:0] spare);
  reg clkseen;
  initial begin par = 1'b0; clkseen = 0; end
  always @(posedge clk) begin
    clkseen <= 1;
    par <= ^wide;
  end
  assign spare = {clkseen, rst};
endmodule

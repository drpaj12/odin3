module muxtest(input clk, input [3:0] a, b, c, d, input s, output reg [3:0] y, z, w);
  always @(posedge clk) begin
    y <= s ? a : b;
    z <= a & c;
    w <= (a == b) ? c : d;
  end
endmodule

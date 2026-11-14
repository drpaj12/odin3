module hint_demo(input clk, input [7:0] a, b, c, input s, output reg [7:0] q, output reg z);
  wire [7:0] m = s ? (a & b) : (a ^ c);
  wire eq = (a == b) & (b != c);
  always @(posedge clk) begin
    q <= eq ? m : (m | {a[3:0], c[3:0]});
    z <= &q | (a[0] & b[1] & c[2] & s);
  end
endmodule

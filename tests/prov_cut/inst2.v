// One module with a mux, instantiated twice; the outputs are registers, so each LUT drives an
// internal net.
module sel(input s, input [3:0] a, b, output [3:0] y);
  assign y = s ? a : b;
endmodule
module inst2(input clk, input s, input [3:0] a, b, c, d, output reg [3:0] y, z);
  wire [3:0] y0, z0;
  sel u0(.s(s), .a(a), .b(b), .y(y0));
  sel u1(.s(s), .a(c), .b(d), .y(z0));
  always @(posedge clk) begin
    y <= y0;
    z <= z0;
  end
endmodule

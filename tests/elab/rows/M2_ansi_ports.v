// M2: ANSI port list — variants: net type (input wire, output wire), range, signed, net type with signed, shared range input [3:0] a, b, output reg with a range
module m2_ansi (
  input wire clk,
  input [3:0] a, b,
  input signed [3:0] s,
  input wire signed [7:0] ws,
  output [3:0] y,
  output reg [3:0] q,
  output wire signed [4:0] z
);
  assign y = a;
  assign z = s;
endmodule

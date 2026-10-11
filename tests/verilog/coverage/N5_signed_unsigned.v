// N5: signed and unsigned — variants: signed and unsigned input ports, output reg signed, wire signed, wire unsigned, reg signed, reg unsigned
module n5_signed_unsigned (input signed [3:0] a, input unsigned [3:0] b, output reg signed [7:0] r);
  wire signed [3:0] w;
  wire unsigned [3:0] u;
  reg signed [7:0] rs;
  reg unsigned [3:0] ru;
  assign w = a;
  assign u = b;
endmodule

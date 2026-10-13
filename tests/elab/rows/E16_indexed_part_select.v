// E16: indexed part-selects — variants: a[s +: 4] and a[s -: 4] with a variable base, a[0 +: 4] with a constant base, r[s +: 2] as a procedural target
module e16_indexed_part (input [7:0] a, input [2:0] s, input [1:0] d, output [3:0] y1, y2, y3, output reg [7:0] r);
  assign y1 = a[s +: 4];
  assign y2 = a[s -: 4];
  assign y3 = a[0 +: 4];
  always @* begin
    r = 8'd0;
    r[s +: 2] = d;
  end
endmodule

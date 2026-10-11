// E15: non-constant bit-select — variants: rvalue a[s] (a mux), lvalue r[s] in a procedural assignment (a decoder), an index expression a[s + 1]
module e15_var_bit_select (input [7:0] a, input [2:0] s, input d, output y1, y2, output reg [7:0] r);
  assign y1 = a[s];
  assign y2 = a[s + 1];
  always @* begin
    r = 8'd0;
    r[s] = d;
  end
endmodule

// expr/mux_empty: $pmux with every B slice undefined -> Y = A
// options: -mux_undef
module t(input [1:0] s, input [1:0] a, output reg [1:0] y);
  always @* case (s)
    2'b01: y = 2'bxx;
    2'b10: y = 2'bxx;
    default: y = a;
  endcase
endmodule

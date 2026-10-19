// 5.2: a wide case (a $pmux with several selects) and a tri-state style conditional.
module cell_mux(input [2:0] s, input [3:0] a, input [3:0] b, input [3:0] c, output reg [3:0] y);
  always @* begin
    case (s)
      3'd0: y = a;
      3'd1: y = b;
      3'd2: y = c;
      3'd3: y = a ^ b;
      default: y = 4'h0;
    endcase
  end
endmodule

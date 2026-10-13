// C3: case with default — variants: case with items and a default last
module c3_case_default (input [1:0] s, input [3:0] a, b, c, output reg [3:0] y);
  always @*
    case (s)
      2'd0: y = a;
      2'd1: y = b;
      2'd2: y = c;
      default: y = 4'd0;
    endcase
endmodule

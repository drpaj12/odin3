// Two outputs driven by one case statement: two $pmux cells from one HDL statement.
module case2(input [1:0] s, input [3:0] a, b, c, d, output reg [3:0] y, z);
  always @(*) begin
    case (s)
      2'd0: begin y = a; z = c; end
      2'd1: begin y = b; z = d; end
      default: begin y = c; z = a; end
    endcase
  end
endmodule

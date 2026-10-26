module case_stmt (input [1:0] s, input [3:0] a, input [3:0] b, output reg [3:0] y);
  always @(*) begin
    case (s)
      2'd0: y = a;
      2'd1: y = b;
      2'd2: y = a & b;
      default: y = 4'd0;
    endcase
  end
endmodule

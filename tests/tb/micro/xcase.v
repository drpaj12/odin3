module xcase(input [2:0] cmd, input [3:0] a, output reg [3:0] y);
  always @* begin
    case (cmd)
      3'd0: y = a;
      3'd1: y = ~a;
      3'bxxx: y = 4'hf;
      3'b1x0, 3'd5: y = 4'h9;
      default: y = 4'h5;
    endcase
  end
endmodule

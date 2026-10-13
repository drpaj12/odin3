// T9 (accepted, 2D-7b review I2): a same-path pair in a later arm, after an exclusive first pair, gets the "simulation and synthesis may differ" warning.
module t9f(input clk, input [1:0] s, input [3:0] d, output reg [3:0] q);
  always @(posedge clk) begin
    (* full_case *)
    case (s)
      2'd0: q = d;
      2'd1: q <= d + 4'd1;
      2'd2: begin q <= d; q = d + 4'd2; end
      default: q <= 4'd7;
    endcase
  end
endmodule

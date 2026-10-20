// FSM micro: fsm_encoding = "binary" on a 4-bit register with 4 states: 4 bits become 2 (rule R-3).
module enc_binary(input clk, input rst, input go, output reg o);
  (* fsm_encoding = "binary" *) reg [3:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 4'd0;
    else case (st)
      4'd0: if (go) st <= 4'd5;
      4'd5: st <= 4'd10;
      4'd10: st <= 4'd15;
      4'd15: st <= 4'd0;
    endcase
  always @* o = (st == 4'd15);
endmodule

// FSM micro: fsm_encoding = "user" keeps the codes but the FSM is still mapped (rules R-1, M-1).
module enc_user(input clk, input rst, input go, output reg o);
  (* fsm_encoding = "user" *) reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule

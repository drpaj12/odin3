// FSM micro: fsm_encoding = "auto" (rules D-6, R-1).
module enc_auto(input clk, input rst, input go, output reg o);
  (* fsm_encoding = "auto" *) reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule

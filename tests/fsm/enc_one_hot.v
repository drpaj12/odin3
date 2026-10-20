// FSM micro: fsm_encoding = "one-hot" forces one-hot (rule R-1; an explicit value beats -encoding).
module enc_one_hot(input clk, input rst, input go, output reg o);
  (* fsm_encoding = "one-hot" *) reg [2:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 3'd0;
    else case (st)
      3'd0: if (go) st <= 3'd1;
      3'd1: st <= 3'd2;
      3'd2: st <= 3'd3;
      3'd3: st <= 3'd0;
    endcase
  always @* o = (st == 3'd3);
endmodule

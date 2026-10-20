// FSM micro: an unknown fsm_encoding string is logged and treated as auto (rule R-1).
module enc_unknown(input clk, input rst, input go, output reg o);
  (* fsm_encoding = "gray" *) reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule

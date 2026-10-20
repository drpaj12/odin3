// FSM micro: fsm_encoding on a 1-bit net is removed with a warning (rule D-2).
module one_bit_attr(input clk, input rst, input go, output reg o);
  (* fsm_encoding = "one-hot" *) reg st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 1'b0;
    else case (st)
      1'b0: if (go) st <= 1'b1;
      1'b1: st <= 1'b0;
    endcase
  always @* o = (st == 1'b1);
endmodule

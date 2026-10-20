// FSM micro: a reader of the state register that is not a compare (an increment with a 2-bit
// result, allowed by rule D-4) keeps the old register alive after fsm. The control outputs that
// carry its next state stay (rule O-1: the unused-output query sees the old register as live).
module live_old_reg(input clk, input rst, input go, output reg o, output [1:0] copy);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
  assign copy = st + 2'd1;
endmodule

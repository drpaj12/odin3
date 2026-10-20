// FSM micro: a default branch back to the reset state makes it self-resetting, not marked (rule D-5).
module self_reset(input clk, input rst, input go, output reg o);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
      default: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule

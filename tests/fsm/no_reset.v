// FSM micro: no reset at all, so no reset state (rules X-2, R-2 without a reset state, O-0).
// Not equivalence-checked: the recoded one-hot register powers up all-zero (spec section 15).
module no_reset(input clk, input go, output reg o);
  reg [1:0] st;
  always @(posedge clk)
    case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule

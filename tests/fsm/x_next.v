// FSM micro: an x next state (rule X-4 "ignored undef transition"). Not equivalence-checked: Yosys
// resolves the x to a concrete next state, which the miter reports as a difference from the x.
module x_next(input clk, input rst, input go, output reg o);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'bxx;
    endcase
  always @* o = (st == 2'd2);
endmodule

// FSM micro: Moore machine, 3 states, async reset (rules D-2, D-3, D-4, X-1 to X-4, R-2, M-1).
module moore_arst(input clk, input rst, input go, output reg o);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule

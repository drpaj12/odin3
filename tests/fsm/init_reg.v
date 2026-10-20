// FSM micro: a register with an init value is not marked (rules D-6, D-7).
module init_reg(input clk, input rst, input go, output reg o);
  reg [1:0] st = 2'd0;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule

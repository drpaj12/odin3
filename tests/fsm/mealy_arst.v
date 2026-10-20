// FSM micro: Mealy machine, output depends on the input and the state (rules X-3, X-4, M-4).
module mealy_arst(input clk, input rst, input a, input b, output reg o);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: st <= a ? 2'd1 : 2'd0;
      2'd1: st <= b ? 2'd2 : 2'd0;
      2'd2: st <= a ? 2'd2 : 2'd1;
    endcase
  always @* o = (st == 2'd1 && b) || (st == 2'd2 && a);
endmodule

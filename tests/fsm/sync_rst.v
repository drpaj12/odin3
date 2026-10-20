// FSM micro: synchronous reset; the reset state is guessed from the mux tree (rules X-2, M-1).
module sync_rst(input clk, input rst, input go, output reg o);
  reg [1:0] st;
  always @(posedge clk)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd1);
endmodule

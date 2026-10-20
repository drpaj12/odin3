// FSM micro: a state register with an enable. proc makes a mux with Q feedback, so fsm after proc
// finds it; if opt ran first it would make a $dffe and nothing would be found (spec F-5).
module enable_reg(input clk, input rst, input en, input go, output reg o);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else if (en)
      case (st)
        2'd0: if (go) st <= 2'd1;
        2'd1: st <= 2'd2;
        2'd2: st <= 2'd0;
      endcase
  always @* o = (st == 2'd2);
endmodule

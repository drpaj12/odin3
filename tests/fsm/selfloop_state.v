// FSM micro: state 3 is never reached from reset but loops on itself. Rule O-0 keeps it: opt_unreachable_states
// looks for any transition into a state, and the self-loop is one (fsm_opt.cc:46-52); 4 states stay.
module selfloop_state(input clk, input rst, input go, output reg o);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
      2'd3: st <= 2'd3;
    endcase
  always @* o = (st == 2'd2) || (st == 2'd3);
endmodule

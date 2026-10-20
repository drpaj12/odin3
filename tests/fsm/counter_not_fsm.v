// FSM micro: a counter is not an FSM (rule D-3: "unexpected cell type", st+1 is not a mux tree).
module counter_not_fsm(input clk, input rst, output o);
  reg [2:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 3'd0;
    else st <= st + 3'd1;
  assign o = (st == 3'd7);
endmodule

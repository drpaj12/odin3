// FSM micro: an FSM in a submodule instantiated twice (rule F-3: per module, not per instance).
module sub_twice_cell(input clk, input rst, input go, output o);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  assign o = (st == 2'd2);
endmodule

module sub_twice(input clk, input rst, input go, output o0, output o1);
  sub_twice_cell u0(.clk(clk), .rst(rst), .go(go), .o(o0));
  sub_twice_cell u1(.clk(clk), .rst(rst), .go(o0), .o(o1));
endmodule

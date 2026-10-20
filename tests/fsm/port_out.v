// FSM micro: the state register is a module output port, so it is not marked (rules D-4, D-6, D-7).
module port_out(input clk, input rst, input go, output reg [1:0] st);
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
endmodule

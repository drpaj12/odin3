// FSM micro: two named wires over one state register (rule X-1, H1). Yosys extracts one FSM per
// register; Odin III nets are merged, so both wires are detected and the second must be skipped.
module alias_wire(input clk, input rst, input go, output reg o);
  reg [1:0] st;
  wire [1:0] w = st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (w == 2'd2);
endmodule

// FSM micro: fsm_encoding = "binary" on a register that is already packed (rule R-3: "existing
// encoding is already a packed binary encoding", the codes stay).
module enc_binary_packed(input clk, input rst, input go, output reg o);
  (* fsm_encoding = "binary" *) reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd3;
      2'd3: st <= 2'd0;
    endcase
  always @* o = (st == 2'd3);
endmodule

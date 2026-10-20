// FSM micro: (* keep *) does not block detection; the old register stays alive (rule D-7, Q3).
module keep_reg(input clk, input rst, input go, output reg o);
  (* keep *) reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule

// FSM micro: active-low async reset (rule M-1: ARST_POLARITY is copied to the new register).
module arst_low(input clk, input rstn, input go, output reg o);
  reg [1:0] st;
  always @(posedge clk or negedge rstn)
    if (!rstn) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd0;
    endcase
  always @* o = (st == 2'd2);
endmodule

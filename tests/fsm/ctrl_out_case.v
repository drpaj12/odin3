// FSM micro: several outputs decoded from the state through case (rules X-3, O-1, M-4).
module ctrl_out_case(input clk, input rst, input go, output reg a, output reg b, output reg c);
  reg [1:0] st;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: st <= 2'd3;
      2'd3: st <= 2'd0;
    endcase
  always @* begin
    a = 1'b0; b = 1'b0; c = 1'b0;
    case (st)
      2'd1: a = 1'b1;
      2'd2: b = 1'b1;
      2'd3: begin a = 1'b1; c = 1'b1; end
    endcase
  end
endmodule

// PR 2D-6 review C1: the common FSM form, `// synopsys full_case` over `<=` arms.
module t(input clk, input [1:0] c, input [7:0] a, output reg [7:0] y4);
  initial y4 = 0;
  always @(posedge clk) case (c) // synopsys full_case
    2'd0: y4 <= a;
    2'd1: y4 <= ~a;
    2'd2: y4 <= a ^ 8'h55;
  endcase
endmodule

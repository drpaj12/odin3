// PR 2D-6 review C1: (* full_case *) over `<=` arms: Yosys adds the empty default case, so the
// unlisted selector value holds (it must not become x).
module t(input clk, input [1:0] c, input [7:0] a, output reg [7:0] y4);
  initial y4 = 0;
  always @(posedge clk) (* full_case *) case (c)
    2'd3: y4 <= a;
  endcase
endmodule

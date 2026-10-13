// Task 22: a case in a clocked block, 8-bit chunks, a multi-expression item and a default.
module t (input clk, input [1:0] sel, input [7:0] a, b, c, output reg [7:0] q);
  always @(posedge clk)
    case (sel)
      2'd0: q <= a;
      2'd1, 2'd2: q <= b;
      default: q <= c;
    endcase
endmodule

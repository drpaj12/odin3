// T1: always @(posedge/negedge …) — variants: posedge, negedge, async reset with or (posedge clk or posedge rst), async reset with a comma (posedge clk, negedge rst_n)
module t1_edges (input clk, rst, rst_n, d, output reg q1, q2, q3, q4);
  always @(posedge clk)
    q1 <= d;
  always @(negedge clk)
    q2 <= d;
  always @(posedge clk or posedge rst)
    if (rst)
      q3 <= 1'b0;
    else
      q3 <= d;
  always @(posedge clk, negedge rst_n)
    if (!rst_n)
      q4 <= 1'b0;
    else
      q4 <= d;
endmodule

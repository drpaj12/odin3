module xasync(input clk, input rst, input en, input srst_n, output reg [3:0] q, output reg [3:0] e, output reg hi);
  always @(posedge clk or negedge rst)
    if (!rst) q <= 4'd0;
    else q <= q + 4'd1;
  always @(posedge clk or negedge rst)
    if (!rst) e <= 4'd9;
    else if (en) e <= e + 4'd1;
  always @(negedge clk or posedge srst_n)
    if (srst_n) hi <= 1'b1;
    else hi <= ~hi;
endmodule

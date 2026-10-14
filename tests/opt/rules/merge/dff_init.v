// merge/dff_init: two $dff cells with the same D, clock and init -> one
module t(input clk, input d, output reg q, output reg r);
  initial q = 1'b1;
  initial r = 1'b1;
  always @(posedge clk) q <= d;
  always @(posedge clk) r <= d;
endmodule

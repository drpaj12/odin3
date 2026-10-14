// merge/dff: two $dff cells with the same D and clock -> one
module t(input clk, input d, output reg q, output reg r);
  always @(posedge clk) q <= d;
  always @(posedge clk) r <= d;
endmodule

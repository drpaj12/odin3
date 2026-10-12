// T19: disable (REJECT).
module m (input clk, a, output reg x);
  always @(posedge clk) begin : blk
    x = a;
    if (a) disable blk;
  end
endmodule

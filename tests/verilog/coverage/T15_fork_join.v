// T15: fork…join (REJECT) — variants: unnamed fork…join, named fork : p … join
module t15_fork_join (input clk, a, b, output reg q, r);
  always @(posedge clk) begin
    fork
      q = a;
      r = b;
    join
    fork : par_blk
      q = b;
    join
  end
endmodule

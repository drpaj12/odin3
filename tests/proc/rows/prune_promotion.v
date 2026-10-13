// prune_promotion: assignments overridden later and promoted by prune
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, en, s, input [3:0] d, e, output reg [3:0] q, r);
  always @(posedge clk) begin
    q <= d;
    r <= e;
    if (en) begin
      q <= e;
      if (s) r <= d; else r <= e;
    end
    q <= q + 4'd1;
  end
endmodule

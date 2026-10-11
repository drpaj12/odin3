// T17: named blocks — variants: begin : name in an always, a named block nested in an unnamed one
module t17_named_blocks (input clk, d, output reg q, r);
  always @(posedge clk) begin : capture
    q <= d;
  end
  always @(posedge clk) begin
    begin : inner
      r <= q;
    end
  end
endmodule

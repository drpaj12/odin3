// F4: forever (REJECT) — variants: forever with a delay, forever with an event control
module f4_forever (input clk, output reg r, q);
  initial begin
    r = 1'b0;
    forever #5 r = ~r;
  end
  initial begin
    q = 1'b0;
    forever @(posedge clk) q = ~q;
  end
endmodule

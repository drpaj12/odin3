// T14: named events (REJECT) — variants: -> e in an always, @(e) waiting on the event
module t14_named_events (input clk, a, output reg y);
  event e;
  always @(posedge clk)
    if (a)
      -> e;
  always @(e)
    y = ~y;
endmodule

// Task 23: a function inlined in a clocked process (no enable).
module t (input clk, input [3:0] d, output reg [3:0] q);
  function [3:0] inc(input [3:0] v);
    inc = v + 1;
  endfunction
  always @(posedge clk) q <= inc(d);
endmodule

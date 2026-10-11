// K12: timing control or nonblocking assignment in a function (REJECT) — variants: a nonblocking assignment, an event control, a delay
module k12_func_timing (input clk, input [3:0] a, output [3:0] y);
  function [3:0] f (input [3:0] x);
    begin
      f <= x;
    end
  endfunction
  function [3:0] g (input [3:0] x);
    begin
      @(posedge clk) g = x;
      #1 g = ~g;
    end
  endfunction
  assign y = f(a) | g(a);
endmodule

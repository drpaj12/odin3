module func_twice (input clk, input [3:0] a, input [3:0] b, output reg [3:0] p, output reg [3:0] q);
  function [3:0] mix;
    input [3:0] u;
    input [3:0] v;
    begin
      if (u[0])
        mix = u ^ v;
      else
        mix = u & v;
    end
  endfunction
  always @(posedge clk) p <= mix(a, b);
  always @(posedge clk) q <= mix(b, a);
endmodule

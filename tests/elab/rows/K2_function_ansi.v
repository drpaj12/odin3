// K2: ANSI function header — variants: several inputs with a shared range (input [3:0] x, z), a scalar input, input integer
module k2_func_ansi (input [3:0] a, b, input s, output [3:0] y1, output [5:0] y2);
  function [3:0] pick (input sel, input [3:0] x, z);
    reg [3:0] r;
    begin
      r = sel ? x : z;
      pick = r;
    end
  endfunction
  function [5:0] widen (input [3:0] x, input integer n);
    integer k;
    begin
      k = n;
      widen = {2'b00, x} << k;
    end
  endfunction
  assign y1 = pick(s, a, b);
  assign y2 = widen(a, 1);
endmodule

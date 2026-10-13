// func_call: function called in a process
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [3:0] a, b, input s, output reg [3:0] y);
  function [3:0] pick(input [3:0] x, z, input c);
    begin
      if (c) pick = x & z; else pick = x | z;
    end
  endfunction
  always @* y = pick(a, b, s);
endmodule

// Task 23: a function with an if inside, called twice, and a case.
module t (input [3:0] a, b, input s, input [1:0] k, output reg [3:0] y, z);
  function [3:0] pick(input [3:0] v, w, input c);
    if (c) pick = v; else pick = w;
  endfunction
  function [3:0] sh(input [3:0] v, input [1:0] n);
    case (n) 2'd0: sh = v; 2'd1: sh = v << 1; default: sh = v >> 1; endcase
  endfunction
  always @* begin
    y = pick(a, b, s);
    z = sh(pick(b, a, s), k);
  end
endmodule

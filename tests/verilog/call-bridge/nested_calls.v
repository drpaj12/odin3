// Task 23: calls inside a function (K6, K11) and a call in a loop.
module t (input [3:0] a, output reg [3:0] y);
  integer i;
  function [3:0] inc(input [3:0] v);
    inc = v + 1;
  endfunction
  function [3:0] inc2(input [3:0] v);
    inc2 = inc(inc(v));
  endfunction
  always @* begin
    y = a;
    for (i = 0; i < 2; i = i + 1) y = inc2(y);
  end
endmodule

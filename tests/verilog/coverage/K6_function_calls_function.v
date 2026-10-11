// K6: a function calling a function — variants: one function called inside another's body
module k6_func_call (input [3:0] a, b, output [3:0] y);
  function [3:0] inc (input [3:0] x);
    inc = x + 1;
  endfunction
  function [3:0] add_inc (input [3:0] x, z);
    add_inc = inc(x) + z;
  endfunction
  assign y = add_inc(a, b);
endmodule

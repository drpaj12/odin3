// Task 23: a call in a continuous assignment: a combinational process of its own.
module t (input [3:0] d, e, output [3:0] y, z);
  function [3:0] mix(input [3:0] v, w);
    mix = (v & w) | (v ^ 4'd5);
  endfunction
  assign y = mix(d, e);
  assign z = mix(e, d) + 4'd1;
endmodule

// K1: non-ANSI function — variants: input declarations in the body, a range return type, a local reg between two inputs, a begin…end body
module k1_func_nonansi (input [3:0] a, b, output [3:0] y);
  function [3:0] add_sat;
    input [3:0] x;
    reg [4:0] t;
    input [3:0] z;
    begin
      t = x + z;
      add_sat = t[4] ? 4'hF : t[3:0];
    end
  endfunction
  assign y = add_sat(a, b);
endmodule

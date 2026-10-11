// K3: function return types — variants: integer, signed with a range, a parameterised range [W-1:0]
module k3_func_ret #(parameter W = 4) (input [W-1:0] a, output [W-1:0] y1, output signed [7:0] y2, output [31:0] y3);
  function integer half (input integer n);
    integer h;
    begin
      h = n / 2;
      half = h;
    end
  endfunction
  function signed [7:0] sext (input signed [3:0] x);
    reg signed [7:0] t;
    begin
      t = x;
      sext = t;
    end
  endfunction
  function [W-1:0] inv (input [W-1:0] x);
    reg [W-1:0] t;
    begin
      t = ~x;
      inv = t;
    end
  endfunction
  assign y1 = inv(a);
  assign y2 = sext(a[3:0]);
  assign y3 = half(W);
endmodule

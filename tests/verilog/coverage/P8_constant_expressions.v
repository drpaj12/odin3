// P8: constant expressions — variants: 2**W-1 in a range, $clog2 in a range, a constant function call in a localparam and in a range, a forward reference to a later localparam
module p8_constant_expressions #(parameter W = 4) (input [2**W-1:0] a, output [$clog2(W)-1:0] y, output [7:0] z);
  localparam A = B + 1;
  localparam B = 2;
  localparam N = width_of(W);
  function integer width_of(input integer n);
    width_of = 2 * n;
  endfunction
  wire [width_of(W)-1:0] t;
  assign y = A;
  assign z = N;
endmodule

// K7: constant functions — variants: a call in a localparam, a call in a range, a call before the function's declaration
module k7_const_func #(parameter N = 10) (input [clog2(N)-1:0] a, output [31:0] y);
  localparam W = clog2(N);
  function integer clog2 (input integer value);
    integer v;
    begin
      v = value - 1;
      for (clog2 = 0; v > 0; clog2 = clog2 + 1)
        v = v >> 1;
    end
  endfunction
  wire [W:0] t;
  assign t = a;
  assign y = W;
endmodule

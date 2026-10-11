// E19: $clog2 in constant context — variants: in a localparam, in a range, in an assign
module e19_clog2 #(parameter N = 16) (output [31:0] y);
  localparam W = $clog2(N);
  wire [$clog2(N)-1:0] c;
  assign y = $clog2(N + 1);
endmodule

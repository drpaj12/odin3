// The model of shiftx_signed.v: Y = A[B +: 2] with the bits outside A reading x.
module s(input [3:0] a, input signed [2:0] b, output [1:0] y);
  assign y = a[b +: 2];
endmodule

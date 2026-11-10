// A `$shiftx` with a signed shift amount, read with -icells: a negative B selects below bit 0.
module s(input [3:0] a, input signed [2:0] b, output [1:0] y);
  \$shiftx #(.A_SIGNED(0), .B_SIGNED(1), .A_WIDTH(4), .B_WIDTH(3), .Y_WIDTH(2)) u (.A(a), .B(b), .Y(y));
endmodule

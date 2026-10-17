// $pow reads each operand's own flag
module divmod_trim_pow(input [3:0] a, input [2:0] b, output [7:0] y);
  assign y = {4'b0000, a} ** {5'b00000, b};
endmodule

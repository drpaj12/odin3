// the zero-extended operand A is cut back (spec 4.3 step 3)
module port_a(input [3:0] a, input [7:0] b, output [7:0] y);
  assign y = {4'b0000, a} + b;
endmodule

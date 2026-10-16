// the zero-extended operand B of a bitwise cell is cut back
module port_b(input [7:0] a, input [3:0] b, output [7:0] y);
  assign y = a ^ {4'b0000, b};
endmodule

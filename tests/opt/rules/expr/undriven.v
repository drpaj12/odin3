// expr/undriven: -undriven: an undriven wire read by a cell -> constant x
// options: -undriven
module t(input [1:0] a, output [1:0] y);
  wire [1:0] w;
  assign y = a & w;
endmodule

module xtri(input oe, input [7:0] d, output [7:0] y);
  assign y = oe ? d : 8'bz;
endmodule

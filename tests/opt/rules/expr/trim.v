// expr/trim: $shiftx whose A has x MSBs above bit 0 -> narrower A
module t(input [1:0] a, input [2:0] b, output y);
  wire [7:0] t = {6'bxxxxxx, a};
  assign y = t[b];
endmodule

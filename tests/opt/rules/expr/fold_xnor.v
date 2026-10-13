// expr/fold_xnor: all-constant $xnor -> its 4-state fold
module t(output [3:0] y);
  wire [3:0] c = 4'b0110;
  wire [3:0] d = 4'b0011;
  assign y = c ~^ d;
endmodule

// expr/fold_pow: all-constant $pow -> its 4-state fold
module t(output [7:0] y);
  wire [3:0] c = 4'd3;
  wire [1:0] d = 2'd2;
  assign y = c ** d;
endmodule

// expr/fold_neg: all-constant $neg -> its 4-state fold
module t(output [3:0] y);
  wire [3:0] c = 4'b0101;
  assign y = -c;
endmodule

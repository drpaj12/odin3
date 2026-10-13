// expr/fold_reduce_or: all-constant $reduce_or -> its 4-state fold
module t(output y);
  wire [3:0] c = 4'b0000;
  assign y = |c;
endmodule

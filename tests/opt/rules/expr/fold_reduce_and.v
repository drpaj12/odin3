// expr/fold_reduce_and: all-constant $reduce_and -> its 4-state fold
module t(output y);
  wire [3:0] c = 4'b1111;
  assign y = &c;
endmodule

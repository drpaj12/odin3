// expr/fold_reduce_xor: all-constant $reduce_xor -> its 4-state fold
module t(output y);
  wire [3:0] c = 4'b0111;
  assign y = ^c;
endmodule

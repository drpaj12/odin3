// expr/fold_reduce_xnor: all-constant $reduce_xnor -> its 4-state fold
module t(output y);
  wire [3:0] c = 4'b0111;
  assign y = ~^c;
endmodule

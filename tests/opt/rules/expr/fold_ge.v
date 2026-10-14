// expr/fold_ge: all-constant $ge -> its 4-state fold
module t(output y);
  wire [3:0] c = 4'd5;
  wire [3:0] d = 4'd9;
  assign y = c >= d;
endmodule

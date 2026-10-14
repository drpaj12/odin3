// expr/fold_logic_or: all-constant $logic_or -> its 4-state fold
module t(output y);
  wire [3:0] c = 4'd5;
  wire [3:0] d = 4'd0;
  assign y = c || d;
endmodule

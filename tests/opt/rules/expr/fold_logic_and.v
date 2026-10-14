// expr/fold_logic_and: all-constant $logic_and -> its 4-state fold
module t(output y);
  wire [3:0] c = 4'd5;
  wire [3:0] d = 4'd0;
  assign y = c && d;
endmodule

// expr/fold_logic_not: all-constant $logic_not -> its 4-state fold
module t(output y);
  wire [3:0] c = 4'b0100;
  assign y = !c;
endmodule

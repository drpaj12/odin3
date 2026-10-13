// expr/fold_mod: all-constant $mod -> its 4-state fold
module t(output [3:0] y);
  wire [3:0] c = 4'd13;
  wire [3:0] d = 4'd3;
  assign y = c % d;
endmodule

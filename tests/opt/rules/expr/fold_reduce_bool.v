// expr/fold_reduce_bool: all-constant $reduce_bool -> its 4-state fold
module t(input [3:0] a, input [3:0] b, output [3:0] y);
  wire [3:0] c = 4'b0000;
  assign y = c ? a : b;
endmodule

// expr/fold_mux: all-constant $mux -> its 4-state fold
module t(input [3:0] a, input [3:0] b, output [3:0] y);
  wire s = 1'b1;
  wire [3:0] c = 4'd5;
  wire [3:0] d = 4'd9;
  assign y = s ? c : d;
endmodule

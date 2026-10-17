// $mod, signed: one 0 stays to carry the sign
module divmod_trim_mod_signed(input signed [4:0] a, input signed [4:0] b, output signed [7:0] y);
  assign y = $signed({3'b000, a[4:0]}) % $signed({2'b00, b[4:0], 1'b0});
endmodule

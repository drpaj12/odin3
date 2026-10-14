// clean/dead_ff: a flip-flop whose output nothing reads is removed
module t(input clk, input d, output y);
  reg q;
  always @(posedge clk) q <= d;
  assign y = d;
endmodule

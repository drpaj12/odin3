// T9 (accepted, 2D-7b): `=` then `<=` to one bit on one control path. Yosys: the last action wins.
module t9_bn (input clk, a, b, output reg q);
  always @(posedge clk) begin
    q = a;
    q <= b;
  end
endmodule

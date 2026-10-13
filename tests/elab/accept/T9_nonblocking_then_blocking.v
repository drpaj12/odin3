// T9 (accepted, 2D-7b): `<=` then `=` to one bit on one control path. Yosys: the last action
// wins (q = a); in simulation the nonblocking update lands last (q = b): they may differ.
module t9_nb (input clk, a, b, output reg q);
  always @(posedge clk) begin
    q <= b;
    q = a;
  end
endmodule

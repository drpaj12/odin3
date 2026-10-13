// err_second_root_if: a second top-level if blocks the reset search
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, rst, en, d, e, output reg q, r);
  // the second top-level if leaves two edge events
  always @(posedge clk or posedge rst) begin if (rst) q <= 0; else q <= d; if (en) r <= e; end
endmodule

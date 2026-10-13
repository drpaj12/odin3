// clocked_unreset: clocked registers without reset, with and without enable
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, en, input [3:0] d, e, output reg [3:0] a, b);
  always @(posedge clk) begin
    if (en) a <= d;
    b <= e;
  end
endmodule

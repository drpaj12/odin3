// latch_nested: latch under nested if
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input s, u, input [3:0] a, b, output reg [3:0] y);
  always @* begin
    y = a;
    if (s) begin
      if (u) y = b;
    end else y = a ^ b;
  end
endmodule

// if_nested: if nested three deep, two outputs
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input s, u, v, input [3:0] a, b, c, output reg [3:0] y, z);
  always @* begin
    z = a;
    if (s) begin
      if (u) begin
        if (v) y = a; else y = b;
      end else y = c;
    end else begin
      y = a ^ b;
      z = c;
    end
  end
endmodule

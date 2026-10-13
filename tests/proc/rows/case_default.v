// case_default: case with default, 8-bit output (one $pmux of WIDTH 8)
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [1:0] s, input [7:0] a, b, c, d, output reg [7:0] y);
  always @* case (s)
    2'd0: y = a;
    2'd1: y = b;
    2'd2: y = c;
    default: y = d;
  endcase
endmodule

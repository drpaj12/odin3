// casex: casex with x and z wildcards
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [3:0] s, input [3:0] a, b, c, d, output reg [3:0] y);
  always @* casex (s)
    4'b1xxx: y = a;
    4'b01xz: y = b;
    4'b001x: y = c;
    default: y = d;
  endcase
endmodule

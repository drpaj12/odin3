// if_chain: if / else if chain with a final else
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [2:0] s, input [3:0] a, b, c, d, output reg [3:0] y);
  always @*
    if (s == 3'd0) y = a;
    else if (s == 3'd1) y = b;
    else if (s[2]) y = c;
    else y = d;
endmodule

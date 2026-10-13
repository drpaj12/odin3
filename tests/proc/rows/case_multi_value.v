// case_multi_value: case items with several values
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [2:0] s, input [3:0] a, b, c, output reg [3:0] y);
  always @* case (s)
    3'd0, 3'd1: y = a;
    3'd2, 3'd3, 3'd4: y = b;
    default: y = c;
  endcase
endmodule

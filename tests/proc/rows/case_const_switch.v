// case_const_switch: case on a constant switch
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [3:0] a, b, c, output reg [3:0] y);
  always @* case (2'd1)
    2'd0: y = a;
    2'd1: y = b;
    default: y = c;
  endcase
endmodule

// case_items_eq_default: case items that assign what the default assigns
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [1:0] s, input [3:0] a, b, output reg [3:0] y);
  always @* case (s)
    2'd0: y = a;
    2'd1: y = b;
    2'd2: y = b;
    default: y = b;
  endcase
endmodule

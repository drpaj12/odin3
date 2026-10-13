// arst_unreset_reg: a register of a reset block that the reset does not assign
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, rst, input [3:0] d, e, output reg [3:0] a, b);
  always @(posedge clk or posedge rst)
    if (rst) a <= 4'd0;
    else begin
      a <= d;
      b <= e;
    end
endmodule

// for_loop: for loop unrolled in a process
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [3:0] a, output reg [3:0] y);
  integer i;
  always @* begin
    for (i = 0; i < 4; i = i + 1) y[i] = a[3 - i];
  end
endmodule

// parallel_case: casez with the parallel_case metacomment
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [2:0] s, input [3:0] a, b, c, d, output reg [3:0] y);
  always @* casez (s) // synopsys parallel_case
    3'b1??: y = a;
    3'b?1?: y = b;
    3'b??1: y = c;
    default: y = d;
  endcase
endmodule

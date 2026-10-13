// full_parallel_case: casez with full_case and parallel_case
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [2:0] s, input [3:0] a, b, c, output reg [3:0] y);
  always @* casez (s) // synopsys full_case parallel_case
    3'b1??: y = a;
    3'b01?: y = b;
    3'b001: y = c;
  endcase
endmodule

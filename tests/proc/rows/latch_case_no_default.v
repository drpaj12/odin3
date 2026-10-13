// latch_case_no_default: latch from a case without default
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [1:0] s, input [3:0] a, b, output reg [3:0] y, z);
  always @* case (s)
    2'd0: begin y = a; z = b; end
    2'd1: y = b;
    2'd2: z = a;
  endcase
endmodule

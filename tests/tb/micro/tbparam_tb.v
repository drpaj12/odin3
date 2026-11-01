// An unsized parameter or localparam of the testbench is a 32-bit integer, not one bit; a ranged one
// keeps its range; $finish under an if ends the run only when the condition holds.
module tbparam_tb;
  parameter N = 3;
  parameter [7:0] B = 200;
  localparam M = N + 40;
  parameter signed [7:0] S = -5;
  integer cycle;
  initial cycle = 0;
  always begin
    #1 cycle = cycle + 1;
    $display("c=%0d eq=%b lt=%b n=%0d b=%0d m=%0d s=%0d", cycle, cycle == N, cycle < N, N, B, M, S);
    if (cycle == N + 1) $finish;
  end
endmodule

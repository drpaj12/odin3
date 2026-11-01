// An unsized parameter or localparam is a 32-bit integer; a sized value keeps its width (BIG, F, NF, L, string S5); a ranged one keeps its range; $finish under an if ends the run only when the condition holds.
module tbparam_tb;
  parameter N = 3;
  parameter [7:0] B = 200;
  localparam M = N + 40;
  parameter signed [7:0] S = -5;
  parameter BIG = 40'h1_0000_0005;
  parameter F = 4'b1010;
  localparam NF = ~F;
  localparam L = {F, 1'b1};
  parameter S5 = "hello";
  integer cycle;
  initial cycle = 0;
  always begin
    #1 cycle = cycle + 1;
    $display("c=%0d eq=%b lt=%b n=%0d b=%0d m=%0d s=%0d", cycle, cycle == N, cycle < N, N, B, M, S);
    $display("big=%h f=%b nf=%b l=%b s5=%s", BIG, F, NF, L, S5);
    if (cycle == N + 1) $finish;
  end
endmodule

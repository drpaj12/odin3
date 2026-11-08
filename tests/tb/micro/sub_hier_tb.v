// hierarchical names into testbench-side instances: reads, writes, array words, task calls,
// nested instances, an absolute path, %m
module sub_hier_tb;
  reg clk;
  integer k;
  bank #(.W(8)) u0 (.clk(clk));
  bank #(.W(4)) u1 (.clk(clk));
  initial clk = 0;
  always #5 clk = ~clk;
  initial begin
    #1;
    $display("u0.w=%0d u1.w=%0d", u0.W, u1.W);
    u0.store(3, 8'h5a);
    u1.store(3, 8'hff);
    $display("u0.mem[3]=%h u1.mem[3]=%h", u0.mem[3], u1.mem[3]);
    k = 3;
    $display("k=%0d u0.mem[k]=%h sum=%0d", k, u0.mem[k], u0.mem[k] + u1.mem[3]);
    u0.mem[2] = 8'h11;
    u0.mode = 1;
    u0.inner0.bump(7);
    sub_hier_tb.u0.inner0.bump(1);
    $display("u0.mem[2]=%h mode=%b inner0.v=%0d", u0.mem[2], u0.mode, u0.inner0.v);
    #20 $display("t=%0t u0.ticks=%0d u1.ticks=%0d", $time, u0.ticks, u1.ticks);
    $finish;
  end
endmodule

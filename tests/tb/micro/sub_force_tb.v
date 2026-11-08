// force of a wire and a reg inside a testbench-side instance, by hierarchical name
module sub_force_tb;
  reg clk;
  model m (.clk(clk));
  initial clk = 0;
  always #5 clk = ~clk;
  initial begin
    force m.debug = 1'b0;
    force m.cpol = clk;
    #12 $display("t=%0t debug=%b cpol=%b flag=%b", $time, m.debug, m.cpol, m.flag);
    release m.cpol;
    #10 $display("t=%0t cpol=%b", $time, m.cpol);
    $finish;
  end
endmodule

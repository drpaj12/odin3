// nested testbench-side instances, tasks of a helper called from the helper, parameters passed down
module sub_nest_tb;
  reg clk;
  wire [7:0] r;
  outer #(.N(3)) o1 (.clk(clk), .r(r));
  outer #(.N(6)) o2 (.clk(clk), .r());
  initial clk = 0;
  always #5 clk = ~clk;
  initial begin
    #26 $display("t=%0t r=%0d", $time, r);
    $finish;
  end
endmodule

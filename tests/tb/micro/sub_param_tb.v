// testbench-side submodule: parameters (positional, named, header style, localparam derived from them)
module sub_param_tb;
  reg clk;
  wire [3:0] q0, q1, q2;
  show #(8, 3) a (.clk(clk), .q());
  show #(.D(5)) b (.clk(clk), .q(q1));
  show c (clk, q2);
  show #(.W(2), .D(1)) d (.clk(clk), .q(q0));
  initial begin
    clk = 0;
    #1 $display("q0=%b q1=%b q2=%b", q0, q1, q2);
    #10 $finish;
  end
endmodule

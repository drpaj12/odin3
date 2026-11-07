// Flip-flops whose enable or synchronous reset is x at the clock edge: an if with an x condition takes
// the else side, so the state must not turn x because opt folded the if into an enable or reset.
module xff_tb;
  reg clk, c, r;
  reg [3:0] d;
  wire [3:0] q1, q2, q3, q4, q5;
  xff dut(.clk(clk), .c(c), .r(r), .d(d), .q1(q1), .q2(q2), .q3(q3), .q4(q4), .q5(q5));
  always #10 clk = ~clk;
  task show; begin
    #1 $display("t=%0t c=%b r=%b d=%b q1=%b q2=%b q3=%b q4=%b q5=%b", $time, c, r, d, q1, q2, q3, q4, q5);
  end endtask
  initial begin
    clk = 0; c = 1'bx; r = 1'bx; d = 4'd9;
    @(posedge clk); show;
    c = 1; r = 0; @(posedge clk); show;
    c = 0; d = 4'd6; @(posedge clk); show;
    c = 1'bx; r = 1'bx; d = 4'd8; @(posedge clk); show;
    c = 1; r = 1; @(posedge clk); show;
    $finish;
  end
endmodule

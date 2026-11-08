// testbench-side submodule: port styles, output reg, ordered/named/empty/constant connections,
// two instances with their own state, a helper clk next to the bench clk
module sub_ports_tb;
  reg clk, rst;
  reg [7:0] d;
  wire [7:0] q1, q2;
  wire busy, par;
  wire [1:0] unused;
  reg [3:0] wide;
  oldstyle u1 (.clk(clk), .d(d), .q(q1), .busy(busy));
  oldstyle u2 (clk, {d[3:0], d[7:4]}, q2, );
  ansistyle u3 (.clk(~clk), .rst(1'b0), .wide(wide), .par(par), .spare(unused));
  initial clk = 0;
  always #5 clk = ~clk;
  initial begin
    d = 8'h12; wide = 4'b0111;
    #1 $display("t=%0t q1=%h q2=%h busy=%b", $time, q1, q2, busy);
    #20 d = 8'hA5;
    #20 $display("t=%0t q1=%h q2=%h busy=%b par=%b spare=%b", $time, q1, q2, busy, par, unused);
    $finish;
  end
endmodule

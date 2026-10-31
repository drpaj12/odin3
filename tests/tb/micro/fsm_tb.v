module fsm_tb;
  reg clk, rst, x;
  wire y;
  wire [1:0] st;
  reg [15:0] pat;
  integer i;
  fsm dut(.clk(clk), .rst(rst), .x(x), .y(y), .st(st));
  always #4 clk = ~clk;
  initial begin
    clk = 0; rst = 1; x = 0; pat = 16'b1011_0110_1101_0011;
    #10 rst = 0;
    for (i = 0; i < 16; i = i + 1) begin
      x = pat[i];
      @(negedge clk);
      $display("%0d: x=%b st=%0d y=%b", i, x, st, y);
    end
    $write("no newline ");
    $write("here\n");
    $finish;
  end
endmodule

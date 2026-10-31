module repeat_tb;
  integer n, c;
  initial begin
    n = -1; c = 0;
    repeat (n) c = c + 1;
    $display("c=%0d", c);
    repeat (-1) c = c + 1;
    $display("c=%0d", c);
    n = 3;
    repeat (n) c = c + 1;
    $display("c=%0d", c);
    $finish;
  end
endmodule

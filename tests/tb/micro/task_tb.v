module task_tb;
  reg [7:0] a, b;
  reg [15:0] sum;
  reg [3:0] n;
  integer errors;
  task add;
    input [7:0] x;
    input [7:0] y;
    output [15:0] z;
    reg [15:0] tmp;
    begin
      tmp = x + y;
      z = tmp;
    end
  endtask
  task check(input [15:0] got, input [15:0] want);
    begin
      if (got !== want) begin
        errors = errors + 1;
        $display("bad %0d vs %0d", got, want);
      end else $display("ok %0d", got);
    end
  endtask
  task pause_and_count;
    inout [3:0] cnt;
    begin
      #5 cnt = cnt + 1;
      check(cnt, 3);
    end
  endtask
  task say; $display("say at %0t", $time); endtask
  initial begin
    errors = 0;
    a = 200; b = 100;
    add(a, b, sum);
    check(sum, 300);
    n = 2;
    pause_and_count(n);
    say;
    add(1, 2, sum);
    check(sum, 3);
    later(7);
    $display("errors=%0d", errors);
    $finish;
  end
  task later(input [7:0] v);
    begin
      #1;
      $display("later %0d at %0t", v, $time);
    end
  endtask
endmodule

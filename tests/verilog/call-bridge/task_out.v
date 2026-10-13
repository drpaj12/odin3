// Task 23: tasks with output arguments, an enable without parentheses (K10), and module variables.
module t (input p, a, b, output reg y, z, w);
  task tk(input x, output o);
    o = ~x;
  endtask
  task both;
    w = a & b;
  endtask
  always @* begin
    tk(p, y);
    tk(a ^ b, z);
    both;
  end
endmodule

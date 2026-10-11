// M11: duplicate names (REJECT) — variants: a port name twice in a non-ANSI header, a port declared twice, two tasks with one name (the duplicate-module form is a tests/verilog/reject case)
module m11_duplicates (a, a, y);
  input a;
  input a;
  output y;
  assign y = a;
  task t;
    begin
    end
  endtask
  task t;
    begin
    end
  endtask
endmodule

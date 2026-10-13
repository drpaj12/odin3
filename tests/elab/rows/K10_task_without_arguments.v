// K10: task enable without an argument list — variants: clr; with no parentheses, a task with no ports and a null-statement body
module k10_task_no_args (input clk, output reg [3:0] q);
  task clr;
    q = 4'd0;
  endtask
  task nop;
    ;
  endtask
  always @(posedge clk) begin
    clr;
    nop;
  end
endmodule

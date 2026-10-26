// A task called from two always blocks, declared after them: TASK_DECL and the call-instance rows.
module task_call (input clk, input [3:0] a, input [3:0] b, output reg [3:0] p, output reg [3:0] q);
  always @(posedge clk) pick(a, b, p);
  always @(posedge clk) pick(b, a, q);
  task pick;
    input [3:0] u;
    input [3:0] v;
    output [3:0] r;
    begin
      if (u[1])
        r = u | v;
      else
        r = u & v;
    end
  endtask
endmodule

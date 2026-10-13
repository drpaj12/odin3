// K9: tasks with ports — variants: ANSI ports with an output, non-ANSI ports in the body, called from an always
module k9_tasks (input clk, input [3:0] a, b, output reg [3:0] s, d);
  task add (input [3:0] x, input [3:0] z, output [3:0] r);
    r = x + z;
  endtask
  task sub;
    input [3:0] x;
    input [3:0] z;
    output [3:0] r;
    begin
      r = x - z;
    end
  endtask
  always @(posedge clk) begin
    add(a, b, s);
    sub(a, b, d);
  end
endmodule

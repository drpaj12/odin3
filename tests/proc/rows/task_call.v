// task_call: task with an output argument called in a process
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input [3:0] a, b, output reg [3:0] y, z);
  task sumdiff(input [3:0] x, w, output [3:0] s, d);
    begin
      s = x + w;
      d = x - w;
    end
  endtask
  always @* sumdiff(a, b, y, z);
endmodule

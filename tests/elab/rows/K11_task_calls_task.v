// K11: a task calling a task and a task call in a for loop — variants: a task enable inside a task body, a task enable inside a for loop
module k11_task_call (input clk, input [7:0] a, output reg [7:0] y);
  integer i;
  task set_bit (input integer n, input v);
    y[n] = v;
  endtask
  task set_low (input v);
    begin
      set_bit(0, v);
      set_bit(1, v);
    end
  endtask
  always @(posedge clk) begin
    for (i = 0; i < 8; i = i + 1)
      set_bit(i, a[i]);
    set_low(1'b0);
  end
endmodule

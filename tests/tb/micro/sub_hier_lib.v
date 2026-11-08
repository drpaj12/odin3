module leafc(input clk);
  integer v;
  initial v = 0;
  task bump;
    input [7:0] n;
    begin
      v = v + n;
      $display("%m bump %0d -> %0d", n, v);
    end
  endtask
endmodule

module bank #(parameter W = 8) (input clk);
  reg [W-1:0] mem [0:7];
  reg mode;
  integer ticks;
  leafc inner0 (.clk(clk));
  initial begin mode = 0; ticks = 0; end
  always @(posedge clk) ticks = ticks + 1;
  task store;
    input [2:0] a;
    input [W-1:0] d;
    begin
      mem[a] = d;
      $display("%m store %0d", a);
    end
  endtask
endmodule

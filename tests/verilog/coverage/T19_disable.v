// T19: disable (REJECT) — variants: disable of the enclosing named block, disable of a task
module t19_disable (input clk, a, output reg [3:0] c);
  task stop_t;
    begin
      disable stop_t;
    end
  endtask
  always @(posedge clk) begin : blk
    if (a)
      disable blk;
    c <= c + 1;
  end
endmodule

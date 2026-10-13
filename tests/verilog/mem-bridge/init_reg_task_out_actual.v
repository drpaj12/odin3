// PR 2D-6 review I1: an initialised register that is the output actual of a task enable in an
// always block is driven by the block (not merged onto the constant).
module t(input clk, output reg [7:0] y0);
  reg [7:0] r = 8'h5;
  task tk(output [7:0] z); z = 7; endtask
  always @(posedge clk) begin tk(r); y0 <= r; end
endmodule

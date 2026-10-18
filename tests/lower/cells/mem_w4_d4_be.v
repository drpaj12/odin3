module m(input clk, input [1:0] we, input [1:0] wa, input [3:0] wd, input [1:0] ra, output [3:0] rd);
  reg [3:0] mem [0:3];
  always @(posedge clk) begin
    if (we[0]) mem[wa][1:0] <= wd[1:0];
    if (we[1]) mem[wa][3:2] <= wd[3:2];
  end
  assign rd = mem[ra];
endmodule

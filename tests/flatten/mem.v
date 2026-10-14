// A memory in a child, instantiated twice.
// golden-formats: il v   (write_blif has no form for a $mem before lower)
module ram (input clk, input we, input [3:0] addr, input [7:0] d, output reg [7:0] q);
  reg [7:0] mem [0:15];
  always @(posedge clk) begin
    if (we) mem[addr] <= d;
    q <= mem[addr];
  end
endmodule
module top (input clk, input we, input [3:0] addr, input [7:0] d, output [7:0] q1, output [7:0] q2);
  ram u1 (.clk(clk), .we(we), .addr(addr), .d(d), .q(q1));
  ram u2 (.clk(clk), .we(~we), .addr(addr), .d(~d), .q(q2));
endmodule

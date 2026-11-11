// registered read data (memory_merge turns it into a synchronous read port), a registered read
// address, and an asynchronous read of the same memory
module xmemsync(input clk, input we, input [2:0] wa, input [2:0] ra, input [7:0] d,
               output reg [7:0] q, output [7:0] q2, output [7:0] qa);
  reg [7:0] m [0:7];
  reg [2:0] rar;
  always @(posedge clk) begin
    if (we) m[wa] <= d;
    q <= m[ra];
    rar <= ra;
  end
  assign q2 = m[rar];
  assign qa = m[ra];
endmodule

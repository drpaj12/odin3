// unmap micro: an 8-bit by 64-word memory, synchronous read and write; partial_map cuts it into
// eight one-bit RAM slices that unmap puts back together (R-6).
module ram_w8 (
    input clk,
    input we,
    input [5:0] addr,
    input [7:0] din,
    output reg [7:0] dout
);
  reg [7:0] mem[0:63];
  always @(posedge clk) begin
    if (we) mem[addr] <= din;
    dout <= mem[addr];
  end
endmodule

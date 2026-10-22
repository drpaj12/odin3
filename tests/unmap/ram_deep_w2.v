// unmap micro: 2 bits by 16 words, synchronous read and write. With tests/unmap/arch_d2.xml (RAM
// blocks of 2 address bits) partial_map splits it into four leaves per data bit; unmap puts each
// bit's leaves back together (R-8). Small enough for equiv-check.
module ram_deep_w2 (
    input clk,
    input we,
    input [3:0] addr,
    input [1:0] din,
    output reg [1:0] dout
);
  reg [1:0] mem[0:15];
  always @(posedge clk) begin
    if (we) mem[addr] <= din;
    dout <= mem[addr];
  end
endmodule

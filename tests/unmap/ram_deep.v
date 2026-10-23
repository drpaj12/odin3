// unmap micro: a 1-bit by 2^17-word memory, synchronous read and write. 17 address bits are above
// the arch's depth limit of 15, so partial_map splits it into four leaves (two levels) with
// write-enable `$and` cells, registered selects and an output `$mux` tree; unmap puts it back
// together (R-8). Too large for equiv-check: tests/unmap/expect.txt lists it as equiv-skip.
module ram_deep (
    input clk,
    input we,
    input [16:0] addr,
    input din,
    output reg dout
);
  reg mem[0:131071];
  always @(posedge clk) begin
    if (we) mem[addr] <= din;
    dout <= mem[addr];
  end
endmodule

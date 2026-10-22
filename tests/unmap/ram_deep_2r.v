// unmap micro: one write port and two read ports on a 2-bit by 16-word memory. With
// tests/unmap/arch_d2.xml partial_map splits every read port's RAM into four leaves; unmap puts
// the leaves back together per read port (R-8).
module ram_deep_2r (
    input clk,
    input we,
    input [3:0] waddr,
    input [3:0] raddr1,
    input [3:0] raddr2,
    input [1:0] din,
    output reg [1:0] dout1,
    output reg [1:0] dout2
);
  reg [1:0] mem[0:15];
  always @(posedge clk) begin
    if (we) mem[waddr] <= din;
    dout1 <= mem[raddr1];
    dout2 <= mem[raddr2];
  end
endmodule

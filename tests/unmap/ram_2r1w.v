// unmap micro: one write port and two read ports on a 4-bit by 16-word memory; partial_map builds
// a RAM per read port with the write port replicated (D8), which unmap puts back together (R-7).
module ram_2r1w (
    input clk,
    input we,
    input [3:0] waddr,
    input [3:0] raddr1,
    input [3:0] raddr2,
    input [3:0] din,
    output reg [3:0] dout1,
    output reg [3:0] dout2
);
  reg [3:0] mem[0:15];
  always @(posedge clk) begin
    if (we) mem[waddr] <= din;
    dout1 <= mem[raddr1];
    dout2 <= mem[raddr2];
  end
endmodule

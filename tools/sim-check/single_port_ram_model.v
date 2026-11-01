// Behavioural model of the VTR primitive single_port_ram (vtr_flow/primitives.v without the
// specify block, which Icarus rejects): synchronous write, registered read-first output; the memory
// starts at 0 (as the simulator's state does).
module single_port_ram #(
    parameter ADDR_WIDTH = 1,
    parameter DATA_WIDTH = 1
) (
    input clk,
    input [ADDR_WIDTH-1:0] addr,
    input [DATA_WIDTH-1:0] data,
    input we,
    output reg [DATA_WIDTH-1:0] out
);
  reg [DATA_WIDTH-1:0] Mem[(1<<ADDR_WIDTH)-1:0];
  integer i;
  initial begin
    for (i = 0; i < (1 << ADDR_WIDTH); i = i + 1) Mem[i] = 0;
    out = 0;
  end
  always @(posedge clk) begin
    if (we) Mem[addr] <= data;
    out <= Mem[addr];
  end
endmodule

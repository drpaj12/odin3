// Soft body of the VTR primitive `single_port_ram` (Odin III's own behavioural description, MIT).
// read_verilog --vtr-compat reads it, as a library unit, only when the architecture record has no
// `single_port_ram` model or an instance's `#()` does not fit the hard cell of that name (4.2 D18,
// #262): Parmys then builds the instance from its own soft module too.
// Write-first is not modelled: the old word is read when the same address is written (read-first),
// as the arch's RAM blocks do.
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
    reg [DATA_WIDTH-1:0] mem [0:(1<<ADDR_WIDTH)-1];

    always @(posedge clk) begin
        if (we) mem[addr] <= data;
        out <= mem[addr];
    end
endmodule

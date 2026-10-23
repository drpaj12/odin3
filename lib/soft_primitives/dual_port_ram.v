// Soft body of the VTR primitive `dual_port_ram` (Odin III's own behavioural description, MIT).
// Read only when the architecture record has no `dual_port_ram` model, or an instance's `#()` does
// not fit the hard cell of that name (4.2 D18, #262). Both ports read the old word; a same-address
// write from both ports lets port 2 win.
module dual_port_ram #(
    parameter ADDR_WIDTH = 1,
    parameter DATA_WIDTH = 1
) (
    input clk,
    input [ADDR_WIDTH-1:0] addr1,
    input [ADDR_WIDTH-1:0] addr2,
    input [DATA_WIDTH-1:0] data1,
    input [DATA_WIDTH-1:0] data2,
    input we1,
    input we2,
    output reg [DATA_WIDTH-1:0] out1,
    output reg [DATA_WIDTH-1:0] out2
);
    reg [DATA_WIDTH-1:0] mem [0:(1<<ADDR_WIDTH)-1];

    always @(posedge clk) begin
        if (we1) mem[addr1] <= data1;
        if (we2) mem[addr2] <= data2;
        out1 <= mem[addr1];
        out2 <= mem[addr2];
    end
endmodule

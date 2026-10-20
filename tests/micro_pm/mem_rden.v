// 4.4 micro (arm_core style): the read is under the write enable, `if (we) out <= RAM[addr]`. The
// read port has an enable that is not constant 1; the RAM has none, so partial_map emulates it
// (a register of the enable and a hold register on the output).
module mem_rden (
    input clk,
    input we,
    input [5:0] addr,
    input [7:0] data,
    output reg [7:0] out
);
    reg [7:0] RAM [63:0];

    always @(posedge clk) begin
        if (we) begin
            RAM[addr] <= data;
            out <= RAM[addr];
        end
    end
endmodule

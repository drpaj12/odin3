// 4.7 micro: a memory whose first index is not 0 (OFFSET != 0), a registered read. partial_map
// leaves such a memory soft.
module mem_offset (
    input clock,
    input [4:0] rd_addr,
    input [4:0] wr_addr,
    input [7:0] wr_data,
    input en,
    output reg [7:0] rd_data
);
    reg [7:0] block_mem [4:20];

    always @(posedge clock) begin
        rd_data <= block_mem[rd_addr];
        if (en) block_mem[wr_addr] <= wr_data;
    end
endmodule

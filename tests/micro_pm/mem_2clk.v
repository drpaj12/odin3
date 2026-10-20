// 4.7 micro: two write ports on two clocks and a registered read. partial_map leaves such a
// memory soft (write ports on several clocks).
module mem_2clk (
    input clock,
    input clock2,
    input [3:0] rd_addr,
    input [3:0] wr_addr,
    input [7:0] wr_data,
    input [3:0] wr_addr2,
    input [7:0] wr_data2,
    output reg [7:0] rd_data
);
    reg [7:0] block_mem [15:0];

    always @(posedge clock) begin
        rd_data <= block_mem[rd_addr];
        block_mem[wr_addr] <= wr_data;
    end
    always @(posedge clock2) block_mem[wr_addr2] <= wr_data2;
endmodule

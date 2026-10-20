// 4.7 micro: a registered read with a variable enable (a `$dffe` on the read data). partial_map
// leaves such a memory soft; memory_merge makes the read port synchronous with RD_EN.
module mem_dffe_var (
    input clock,
    input [3:0] rd_addr,
    input [3:0] wr_addr,
    input [7:0] wr_data,
    input en,
    input rd_en,
    output reg [7:0] rd_data
);
    reg [7:0] block_mem [15:0];

    always @(posedge clock) begin
        if (rd_en) rd_data <= block_mem[rd_addr];
        if (en) block_mem[wr_addr] <= wr_data;
    end
endmodule

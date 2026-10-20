// 4.7 micro: a registered-read RAM (16 x 8). `memory_merge` makes the read register part of the
// `$mem` read port (RD_CLK_ENABLE 1); the write is on the same clock.
module mem_regread (
    input clock,
    input [3:0] rd_addr,
    input [3:0] wr_addr,
    input [7:0] wr_data,
    input en,
    output reg [7:0] rd_data
);
    reg [7:0] block_mem [15:0];

    always @(posedge clock) begin
        rd_data <= block_mem[rd_addr];
        if (en) block_mem[wr_addr] <= wr_data;
    end
endmodule

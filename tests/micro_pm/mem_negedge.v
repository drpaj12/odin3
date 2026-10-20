// 4.7 micro: a read register on the falling edge, the write on the rising edge. partial_map leaves
// such a memory soft; memory_merge makes the read port synchronous on the falling edge.
module mem_negedge (
    input clock,
    input [3:0] rd_addr,
    input [3:0] wr_addr,
    input [7:0] wr_data,
    input en,
    output reg [7:0] rd_data
);
    reg [7:0] block_mem [15:0];

    always @(negedge clock) rd_data <= block_mem[rd_addr];
    always @(posedge clock) if (en) block_mem[wr_addr] <= wr_data;
endmodule

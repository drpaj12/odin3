// 4.7 micro: the same RAM as mem_regread with a ram_style attribute (G-3: ignored, one warning).
module mem_ramstyle (
    input clock,
    input [3:0] rd_addr,
    input [3:0] wr_addr,
    input [7:0] wr_data,
    input en,
    output reg [7:0] rd_data
);
    (* ram_style = "block" *) reg [7:0] block_mem [15:0];

    always @(posedge clock) begin
        rd_data <= block_mem[rd_addr];
        if (en) block_mem[wr_addr] <= wr_data;
    end
endmodule

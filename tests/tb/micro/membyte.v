// Byte-enable writes of a memory word in an unrolled loop (amber's generic_sram_byte_en shape and
// the `+:` form): a write of one byte must leave the other bytes of the word as they were.
module membyte (
    input clk,
    input we,
    input [1:0] a,
    input [3:0] be,
    input [31:0] d,
    output reg [31:0] qa,
    output reg [31:0] qb
);
    reg [31:0] ma [0:3];
    reg [31:0] mb [0:3];
    integer i;
    always @(posedge clk) begin
        qa <= ma[a];
        qb <= mb[a];
        if (we) begin
            for (i = 0; i < 4; i = i + 1) begin
                ma[a][i*8+0] <= be[i] ? d[i*8+0] : ma[a][i*8+0];
                ma[a][i*8+1] <= be[i] ? d[i*8+1] : ma[a][i*8+1];
                ma[a][i*8+2] <= be[i] ? d[i*8+2] : ma[a][i*8+2];
                ma[a][i*8+3] <= be[i] ? d[i*8+3] : ma[a][i*8+3];
                ma[a][i*8+4] <= be[i] ? d[i*8+4] : ma[a][i*8+4];
                ma[a][i*8+5] <= be[i] ? d[i*8+5] : ma[a][i*8+5];
                ma[a][i*8+6] <= be[i] ? d[i*8+6] : ma[a][i*8+6];
                ma[a][i*8+7] <= be[i] ? d[i*8+7] : ma[a][i*8+7];
                if (be[i]) mb[a][i*8 +: 8] <= d[i*8 +: 8];
            end
        end
    end
endmodule

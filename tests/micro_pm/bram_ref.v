// The reference of tests/micro/verilog/common/bram.v for equiv-check (4.4 R-10): the block RAM
// written out (write port 1, read port 2; the write happens when the read enable is 0) with the
// read enable emulated by hand.
module block_ram (
    input clock,
    input [7:0] rd_addr,
    input rd_en,
    input [7:0] wr_addr,
    input [7:0] wr_data,
    input wr_en,
    output [7:0] rd_data
);
    wire [7:0] ram_out;
    reg en_r;
    reg [7:0] hold;

    dual_port_ram #(.ADDR_WIDTH(8), .DATA_WIDTH(8)) u (
        .clk(clock), .addr1(wr_addr), .data1(wr_data), .we1(~rd_en & wr_en), .out1(),
        .addr2(rd_addr), .data2(8'b0), .we2(1'b0), .out2(ram_out));

    always @(posedge clock) begin
        en_r <= rd_en;
        hold <= rd_data;
    end
    assign rd_data = en_r ? ram_out : hold;
endmodule

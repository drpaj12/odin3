// The reference of tests/micro/verilog/common/rom.v for equiv-check (4.4 R-10): the ROM block
// written out (single-port RAM, write enable 0) with the read enable emulated by hand.
module rom (
    input clock,
    input [3:0] rd_addr,
    input rd_en,
    output [7:0] rd_data
);
    wire [7:0] ram_out;
    reg en_r;
    reg [7:0] hold;

    single_port_ram #(.ADDR_WIDTH(4), .DATA_WIDTH(8)) u (
        .clk(clock), .addr(rd_addr), .data(8'b0), .we(1'b0), .out(ram_out));

    always @(posedge clock) begin
        en_r <= rd_en;
        hold <= rd_data;
    end
    assign rd_data = en_r ? ram_out : hold;
endmodule

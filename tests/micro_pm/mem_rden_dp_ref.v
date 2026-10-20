// The reference of mem_rden_dp for equiv-check: the RAM block written out (write port 1, read port
// 2) with the emulation of the read enable by hand. Same ports as mem_rden_dp.
module mem_rden_dp (
    input clk,
    input wen,
    input ren,
    input [4:0] waddr,
    input [4:0] raddr,
    input [3:0] data,
    output [3:0] q
);
    wire [3:0] ram_out;
    reg en_r;
    reg [3:0] hold;

    dual_port_ram #(.ADDR_WIDTH(5), .DATA_WIDTH(4)) u (
        .clk(clk), .addr1(waddr), .data1(data), .we1(wen), .out1(),
        .addr2(raddr), .data2(4'b0), .we2(1'b0), .out2(ram_out));

    always @(posedge clk) begin
        en_r <= ren;
        hold <= q;
    end
    assign q = en_r ? ram_out : hold;
endmodule

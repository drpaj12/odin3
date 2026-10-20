// The reference of mem_rden for equiv-check: the RAM block written out, with the emulation of the
// read enable written by hand (a registered enable, a hold register, a mux). Same ports as mem_rden.
module mem_rden (
    input clk,
    input we,
    input [5:0] addr,
    input [7:0] data,
    output [7:0] out
);
    wire [7:0] ram_out;
    reg en_r;
    reg [7:0] hold;

    // write on port 1, read on port 2, as partial_map maps a memory whose write address has its own
    // logic
    dual_port_ram #(.ADDR_WIDTH(6), .DATA_WIDTH(8)) u (
        .clk(clk), .addr1(addr), .data1(data), .we1(we), .out1(),
        .addr2(addr), .data2(8'b0), .we2(1'b0), .out2(ram_out));

    always @(posedge clk) begin
        en_r <= we;
        hold <= out;
    end
    assign out = en_r ? ram_out : hold;
endmodule

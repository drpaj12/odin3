// The reference of tests/micro/verilog/common/nr.v for equiv-check (4.4 R-10): three read-only
// ports, each its own single-port RAM block with the read enable emulated by hand. The enables are
// those of the if / else if chain: en1, ~en1 & en2, ~en1 & ~en2 & en3.
module twoR_twoW (
    input clock,
    input en1,
    input en2,
    input en3,
    input [3:0] addr1,
    input [3:0] addr2,
    input [3:0] addr3,
    output [7:0] rd_data1,
    output [7:0] rd_data2,
    output [7:0] rd_data3
);
    wire [7:0] ram1;
    wire [7:0] ram2;
    wire [7:0] ram3;
    reg r1, r2, r3;
    reg [7:0] h1, h2, h3;

    single_port_ram #(.ADDR_WIDTH(4), .DATA_WIDTH(8)) u1 (
        .clk(clock), .addr(addr1), .data(8'b0), .we(1'b0), .out(ram1));
    single_port_ram #(.ADDR_WIDTH(4), .DATA_WIDTH(8)) u2 (
        .clk(clock), .addr(addr2), .data(8'b0), .we(1'b0), .out(ram2));
    single_port_ram #(.ADDR_WIDTH(4), .DATA_WIDTH(8)) u3 (
        .clk(clock), .addr(addr3), .data(8'b0), .we(1'b0), .out(ram3));

    always @(posedge clock) begin
        r1 <= en1;
        r2 <= ~en1 & en2;
        r3 <= ~en1 & ~en2 & en3;
        h1 <= rd_data1;
        h2 <= rd_data2;
        h3 <= rd_data3;
    end
    assign rd_data1 = r1 ? ram1 : h1;
    assign rd_data2 = r2 ? ram2 : h2;
    assign rd_data3 = r3 ? ram3 : h3;
endmodule

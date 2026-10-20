// 4.2 B-1, B-2: instances of generic blocks of tests/unit/data/arch/generic_blocks.xml, every
// port connected at the model's width.
module generic_ok (
    input clk,
    input rst,
    input we,
    input [7:0] a,
    input [7:0] b,
    input [6:0] waddr,
    input [6:0] raddr,
    input [3:0] rx,
    output [15:0] prod,
    output [7:0] dout,
    output [3:0] tx,
    output link_up
);
  mult_8 u_mul (.a(a), .b(b), .out(prod));
  dpram_128x8 u_ram (.clk(clk), .we(we), .waddr(waddr), .din(a), .raddr(raddr), .dout(dout));
  pcie u_pcie (.clk(clk), .rst(rst), .rx(rx), .tx(tx), .link_up(link_up));
endmodule

// A `$mem` read with -icells: one transparent synchronous read port with an enable and a write port
// with two byte enables. write_verilog --quartus writes it as a reg array with a write bypass.
module t(input clk, input we, input re, input [1:0] wa, input [1:0] ra, input [3:0] d, input [1:0] be, output [3:0] q);
  \$mem #(.MEMID("m"), .SIZE(4), .OFFSET(0), .ABITS(2), .WIDTH(4), .RD_PORTS(1), .WR_PORTS(1),
    .RD_CLK_ENABLE(1'b1), .RD_CLK_POLARITY(1'b1), .RD_TRANSPARENT(1'b1), .WR_CLK_ENABLE(1'b1),
    .WR_CLK_POLARITY(1'b1), .INIT("")) u (.RD_CLK(clk), .RD_EN(re), .RD_ADDR(ra), .RD_DATA(q),
    .WR_CLK(clk), .WR_EN({{2{be[1] & we}}, {2{be[0] & we}}}), .WR_ADDR(wa), .WR_DATA(d));
endmodule

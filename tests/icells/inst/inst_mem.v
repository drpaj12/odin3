// 5.2 inst level: one \$mem instance with non-default parameters (read with -icells).
module inst_mem(
  input rd_clk,
  input rd_en,
  input [3:0] rd_addr,
  output [7:0] rd_data,
  input wr_clk,
  input [7:0] wr_en,
  input [3:0] wr_addr,
  input [7:0] wr_data
);
  \$mem #(.MEMID("m"), .SIZE(16), .OFFSET(0), .ABITS(4), .WIDTH(8), .RD_PORTS(1), .WR_PORTS(1), .RD_CLK_ENABLE(1'b1), .RD_CLK_POLARITY(1'b0), .RD_TRANSPARENT(1'b0), .WR_CLK_ENABLE(1'b1), .WR_CLK_POLARITY(1'b1), .INIT(16'h05AF)) u (
    .RD_CLK(rd_clk),
    .RD_EN(rd_en),
    .RD_ADDR(rd_addr),
    .RD_DATA(rd_data),
    .WR_CLK(wr_clk),
    .WR_EN(wr_en),
    .WR_ADDR(wr_addr),
    .WR_DATA(wr_data)
  );
endmodule

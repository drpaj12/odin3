// 5.2 inst level: one \$memwr instance with non-default parameters (read with -icells).
module inst_memwr(
  input clk,
  input [7:0] en,
  input [3:0] addr,
  input [7:0] data
);
  \$memwr #(.MEMID("w"), .ABITS(4), .WIDTH(8), .CLK_ENABLE(1), .CLK_POLARITY(1), .PRIORITY(3)) u (
    .CLK(clk),
    .EN(en),
    .ADDR(addr),
    .DATA(data)
  );
endmodule

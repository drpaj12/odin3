// 5.2 inst level: one \$memrd instance with non-default parameters (read with -icells).
module inst_memrd(
  input clk,
  input en,
  input [3:0] addr,
  output [7:0] data
);
  \$memrd #(.MEMID("r"), .ABITS(4), .WIDTH(8), .CLK_ENABLE(1), .CLK_POLARITY(0), .TRANSPARENT(0)) u (
    .CLK(clk),
    .EN(en),
    .ADDR(addr),
    .DATA(data)
  );
endmodule

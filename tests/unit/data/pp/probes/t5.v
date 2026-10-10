`define B `ifdef X 1 `else 2 `endif
module m; wire [3:0] w = `B; endmodule

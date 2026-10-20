// 4.4 micro: an explicit single_port_ram of 9 address bits, 1 data bit: only padded to D.
module ram_a9 (input clk, input [8:0] addr, input data, input we, output out);
  single_port_ram #(.ADDR_WIDTH(9), .DATA_WIDTH(1)) u (.clk(clk), .addr(addr), .data(data), .we(we), .out(out));
endmodule

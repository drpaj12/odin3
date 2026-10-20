// 4.4 micro: an explicit single_port_ram of 13 address bits and 1 data bit. With a depth limit D
// of 11 (COFFE) partial_map splits it into 2^(13-11) = 4 blocks; with D = 15 (k6) it only pads it.
module ram_a13_sp (input clk, input [12:0] addr, input data, input we, output out);
  single_port_ram #(.ADDR_WIDTH(13), .DATA_WIDTH(1)) u (.clk(clk), .addr(addr), .data(data), .we(we), .out(out));
endmodule

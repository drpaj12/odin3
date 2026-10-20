module ram_a11 (input clk, input [10:0] addr, input data, input we, output out);
  single_port_ram #(.ADDR_WIDTH(11), .DATA_WIDTH(1)) u (.clk(clk), .addr(addr), .data(data), .we(we), .out(out));
endmodule

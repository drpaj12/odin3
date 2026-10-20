// 4.4 micro: an explicit dual_port_ram of 13 address bits and 1 data bit (4 blocks at D = 11).
module ram_a13_dp (input clk, input [12:0] addr1, input [12:0] addr2, input data1, input data2,
                   input we1, input we2, output out1, output out2);
  dual_port_ram #(.ADDR_WIDTH(13), .DATA_WIDTH(1)) u (.clk(clk), .addr1(addr1), .addr2(addr2),
      .data1(data1), .data2(data2), .we1(we1), .we2(we2), .out1(out1), .out2(out2));
endmodule

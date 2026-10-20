// 5.1 micro: a simple dual-port RAM, one write address and one read address, registered read
// (maps to altsyncram DUAL_PORT).
module altsyncram_sdp_micro(input clk, input we, input [3:0] waddr, input [3:0] raddr,
                            input [7:0] din, output reg [7:0] dout);
  reg [7:0] mem [0:15];
  always @(posedge clk) begin
    if (we) mem[waddr] <= din;
    dout <= mem[raddr];
  end
endmodule

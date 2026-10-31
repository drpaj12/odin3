module mem(input clk, input we, input [2:0] addr, input [7:0] din, output [7:0] dout);
  reg [7:0] m [0:7];
  reg [2:0] ra;
  always @(posedge clk) begin
    if (we) m[addr] <= din;
    ra <= addr;
  end
  assign dout = m[ra];
endmodule

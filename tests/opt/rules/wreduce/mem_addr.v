// $mem: an address wider than ceil_log2(OFFSET + SIZE) loses its top bits
module mem_addr(input clk, input [7:0] addr, input [7:0] d, input we, output reg [7:0] q);
  reg [7:0] mem [0:9];
  always @(posedge clk) begin
    if (we) mem[addr] <= d;
    q <= mem[addr];
  end
endmodule

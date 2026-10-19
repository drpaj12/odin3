// 5.1 micro: memories that must stay soft (spec R-6, H1): a read register with an initial value,
// a variable read enable, an asynchronous read, and two write ports. None maps to altsyncram.
module altsyncram_soft_micro(input clk, input we, input we2, input ren, input [3:0] addr,
                             input [3:0] addr2, input [7:0] din,
                             output reg [7:0] q_init = 8'hAA, output reg [7:0] q_en,
                             output [7:0] q_async, output reg [7:0] q_two);
  reg [7:0] m1 [0:15];
  reg [7:0] m2 [0:15];
  reg [7:0] m3 [0:15];
  reg [7:0] m4 [0:15];
  always @(posedge clk) begin
    if (we) m1[addr] <= din;
    q_init <= m1[addr];
  end
  always @(posedge clk) begin
    if (we) m2[addr] <= din;
    if (ren) q_en <= m2[addr];
  end
  always @(posedge clk) if (we) m3[addr] <= din;
  assign q_async = m3[addr2];
  always @(posedge clk) begin
    if (we) m4[addr] <= din;
    if (we2) m4[addr2] <= ~din;
    q_two <= m4[addr];
  end
endmodule

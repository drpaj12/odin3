// 6 words behind a 3-bit address: addresses 6 and 7 reach no word (read x, write dropped);
// x address bits on a read; a partial write enable (low nibble only)
module xmemoor(input clk, input we, input lo, input [2:0] wa, input [2:0] ra, input [7:0] d,
              output [7:0] qa, output reg [7:0] q);
  reg [7:0] m [0:5];
  always @(posedge clk) begin
    if (we) m[wa] <= d;
    if (lo) m[wa][3:0] <= d[7:4];
    q <= m[ra];
  end
  assign qa = m[ra];
endmodule

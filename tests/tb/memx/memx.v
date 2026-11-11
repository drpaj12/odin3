// two write ports, an asynchronous read, a registered read (memory_merge makes it a synchronous
// port) and a ROM-like second memory: the DUT of the x-input check (lower and memory cell agree)
module memx(input clk, input we, input we2, input [2:0] wa, input [2:0] wa2, input [2:0] ra,
            input [3:0] d, input [3:0] d2, input [3:0] be, output [3:0] qa, output reg [3:0] q);
  reg [3:0] m [0:5];
  integer i;
  initial for (i = 0; i < 6; i = i + 1) m[i] = i;
  always @(posedge clk) begin
    if (we) begin
      if (be[0]) m[wa][0] <= d[0];
      if (be[1]) m[wa][1] <= d[1];
      if (be[2]) m[wa][2] <= d[2];
      if (be[3]) m[wa][3] <= d[3];
    end
    if (we2) m[wa2] <= d2;
    q <= m[ra];
  end
  assign qa = m[ra];
endmodule

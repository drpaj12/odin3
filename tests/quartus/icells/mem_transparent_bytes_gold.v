// The behavioural model of mem_transparent_bytes.v (a transparent synchronous read with a read enable
// and a byte-wide write enable), for quartus-check --gold.
module t(input clk, input we, input re, input [1:0] wa, input [1:0] ra, input [3:0] d, input [1:0] be, output reg [3:0] q);
  reg [3:0] m [0:3];
  wire [1:0] en = be & {2{we}};
  always @(posedge clk) begin
    if (en[0]) m[wa][1:0] <= d[1:0];
    if (en[1]) m[wa][3:2] <= d[3:2];
    if (re) begin
      q <= m[ra];
      if (en[0] && wa == ra) q[1:0] <= d[1:0];
      if (en[1] && wa == ra) q[3:2] <= d[3:2];
    end
  end
endmodule

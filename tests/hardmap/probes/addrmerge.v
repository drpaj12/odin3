// R-8 address merge (3C agent default d): the read address is registered first, so memory_dff turns
// the read into a synchronous port. In the RTL the registered address reads the memory AFTER the
// write of the same edge (new data on a same-address write); VTR's RAMs are read-first (old data).
module addrmerge (input clk, input we, input [3:0] wa, input [3:0] ra, input [7:0] d,
  output [7:0] out);
  reg [7:0] m [0:15];
  reg [3:0] ra_q;
  always @(posedge clk) begin
    if (we) m[wa] <= d;
    ra_q <= ra;
  end
  assign out = m[ra_q];
endmodule

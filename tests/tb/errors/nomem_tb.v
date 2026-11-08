// EXPECT: has no word array `rom`
// DUT: ../micro/poke.v
// A memory is named only by `lower --keep-mem`: without it the load into the DUT is refused.
module nomem_tb;
  reg clk;
  wire [7:0] data, q;
  poke uut (.clk(clk), .addr(3'd0), .we(1'b0), .din(8'd0), .data(data), .q(q));
  initial begin
    clk = 0;
    $readmemh("mem.hex", uut.rom);
    $finish;
  end
endmodule

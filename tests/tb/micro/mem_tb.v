module mem_tb;
  reg clk, we;
  reg [2:0] addr;
  reg [7:0] din;
  wire [7:0] dout;
  reg [7:0] stim [0:7];
  integer i;
  mem dut(.clk(clk), .we(we), .addr(addr), .din(din), .dout(dout));
  always #5 clk = ~clk;
  initial begin
    clk = 0; we = 0; addr = 0; din = 0;
    $readmemh("mem.hex", stim);
    for (i = 0; i < 8; i = i + 1) begin
      @(negedge clk);
      we = 1; addr = i; din = stim[i];
    end
    @(negedge clk) we = 0;
    for (i = 0; i < 8; i = i + 1) begin
      addr = 7 - i;
      @(negedge clk);
      @(negedge clk);
      $display("addr=%0d dout=%h %b", addr, dout, dout);
    end
    $finish;
  end
endmodule

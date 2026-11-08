// $readmemh into a DUT memory, hierarchical reads with a variable index, a part select of a word
module poke_tb;
  reg clk;
  reg [2:0] addr;
  reg we;
  reg [7:0] din;
  wire [7:0] data, q;
  integer i;
  poke uut (.clk(clk), .addr(addr), .we(we), .din(din), .data(data), .q(q));
  always #5 clk = ~clk;
  initial begin
    clk = 0; we = 0; din = 0; addr = 0;
    #1 $display("before %h", data);
    $readmemh("mem.hex", uut.rom);
    $display("after %h", data);
    for (i = 0; i < 8; i = i + 1) begin
      addr = i;
      #1 $display("rom[%0d] = %h data %h low %h", i, uut.rom[i], data, uut.rom[i][3:0]);
    end
    for (i = 0; i < 8; i = i + 1) begin
      addr = i; din = uut.rom[i] + 1; we = 1;
      @(posedge clk); #1;
    end
    we = 0;
    for (i = 0; i < 8; i = i + 1) begin
      addr = i;
      #1 $display("ram[%0d] = %h q %h bit7 %b", i, uut.ram[i], q, uut.ram[i][7]);
    end
    $finish;
  end
endmodule

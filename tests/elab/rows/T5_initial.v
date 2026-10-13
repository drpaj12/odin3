// T5: initial — variants: constant initial value of a reg, a for loop filling a memory, $readmemh and $readmemb, other content (a delay, $display) that 2D ignores
module t5_initial (input clk, input [3:0] addr, output [7:0] y);
  reg [7:0] r;
  reg [7:0] mem [0:15];
  reg [7:0] rom [0:15];
  integer i;
  initial r = 8'd5;
  initial begin
    for (i = 0; i < 16; i = i + 1)
      mem[i] = 8'd0;
  end
  initial $readmemh("t5_init.hex", rom);
  initial $readmemb("t5_init.bin", rom, 0, 7);
  initial begin
    #10;
    $display("t5 %d", r);
  end
  assign y = mem[addr] ^ rom[addr] ^ r;
endmodule

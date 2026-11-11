module meminit_tb;
  reg clk, we;
  reg [1:0] wa, ra;
  reg [3:0] d;
  wire [3:0] rom_q, ram_q;
  integer i;
  meminit dut(.clk(clk), .we(we), .wa(wa), .ra(ra), .d(d), .rom_q(rom_q), .ram_q(ram_q));
  always #5 clk = ~clk;
  initial begin
    clk = 0; we = 0; wa = 0; ra = 0; d = 0;
    for (i = 0; i < 4; i = i + 1) begin
      ra = i; #1 $display("rom[%0d]=%h", i, rom_q);
      @(posedge clk); #1 $display("ram[%0d]=%h", i, ram_q);
    end
    for (i = 0; i < 4; i = i + 1) begin
      @(negedge clk); we = 1; wa = i; d = 4'hf - i; ra = i;
      @(posedge clk); #1 $display("w%0d ram_q=%h rom_q=%h", i, ram_q, rom_q);
    end
    we = 0;
    for (i = 0; i < 4; i = i + 1) begin
      ra = i; @(posedge clk); #1 $display("r%0d ram_q=%h", i, ram_q);
    end
    $finish;
  end
endmodule

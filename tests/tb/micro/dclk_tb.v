`timescale 1 ns / 1 ns
module dclk_tb;
  parameter CYCLES = 80;
  reg clk, en_in, sel;
  reg [7:0] d;
  integer cycle;
  wire [7:0] a, b, c, e;
  wire div, div4;
  dclk dut (.clk(clk), .en_in(en_in), .sel(sel), .d(d), .a(a), .b(b), .c(c), .e(e), .div(div),
            .div4(div4));
  initial begin clk = 0; en_in = 0; sel = 0; d = 8'd1; cycle = 0; end
  always #5 clk = ~clk;
  always @(negedge clk) begin
    $display("%0d %h %h %h %h %b %b", cycle, a, b, c, e, div, div4);
    #2;
    en_in = (cycle % 7) < 4;
    sel = (cycle / 20) % 2;
    d = (cycle * 5 + 3) & 8'hff;
    cycle = cycle + 1;
    if (cycle == CYCLES) $finish;
  end
endmodule

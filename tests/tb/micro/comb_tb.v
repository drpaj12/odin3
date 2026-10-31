`timescale 1ns/1ps
module comb_tb;
  reg [3:0] a, b;
  reg sel;
  wire [4:0] sum;
  wire [3:0] mx;
  wire eq;
  integer i;
  comb dut(.a(a), .b(b), .sel(sel), .sum(sum), .mx(mx), .eq(eq));
  initial begin
    a = 0; b = 0; sel = 0;
    for (i = 0; i < 20; i = i + 1) begin
      a = i * 7;
      b = i * 3 + 1;
      sel = i[0];
      #10;
      $display("%0d i=%0d a=%d b=%d sel=%b sum=%d mx=%h eq=%b", $time, i, a, b, sel, sum, mx, eq);
    end
    $display("done %0d", i);
    $finish;
  end
endmodule

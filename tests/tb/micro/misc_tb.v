module misc_tb;
  integer i, s;
  reg [7:0] r;
  reg signed [7:0] sr;
  reg [31:0] w;
  wire [7:0] inv;
  assign inv = ~r;
  initial begin
    $monitor("mon %0d r=%d inv=%d", i, r, inv);
    i = 0; r = 8'd250;
    #1 r = r + 3;
    #1 r = r + 10;
    sr = -5;
    $display("sr=%d %0d %h", sr, sr, sr);
    $display("shift %d %d", sr >>> 1, r >> 1);
    w = {r, 4'hA, 4'h5, 16'hBEEF};
    $display("%h %x %o %b", w, w, r, r);
    $display("cond %0d", (r > 8'd5) ? 10 : 20);
    r = 0;
    #1;
    $display("literal", " strings ", 42, 43);
    $display("%5d|%5d|%05d|%s", 7, 7, 7, "str");
    $strobe("strobe %0d", r);
    r = 9;
    $finish;
  end
endmodule

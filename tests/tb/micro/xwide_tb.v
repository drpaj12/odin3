// Values wider than 64 bits: arithmetic, shifts, compares, concatenation, selects and formats at
// 70, 130 and 512 bits, and a DUT with 512-bit ports.
module xwide_tb;
  reg [69:0] a, b;
  reg [129:0] c, d;
  reg signed [129:0] sc;
  reg [511:0] e, f;
  reg clk;
  wire [511:0] s, r;
  integer i;
  xwide dut(.clk(clk), .a(e), .b(f), .s(s), .r(r));
  initial begin
    a = 70'h3f_ffff_ffff_ffff_ffff;
    b = 70'd1;
    $display("a+b %h %d", a + b, a + b);
    $display("a-b %h a*a %h", a - b, a * a);
    $display("a/b %h a%%7 %0d a/3 %d", a / b, a % 7, a / 3);
    $display("shl %h shr %h", a << 5, a >> 66);
    $display("cmp %b %b %b %b", a > b, a == b, a != b, a === a);
    $display("cat %h", {a[3:0], b, 4'hf});
    $display("bit %b %b %b", a[69], a[65], b[0]);
    a[67:60] = 8'bx01z0000;
    $display("x70 %h %b", a, a[69:56]);
    $display("x70 %d %b %b", a, a == b, a + b);
    c = {2'b10, 64'hdead_beef_0123_4567, 64'h89ab_cdef_fedc_ba98};
    d = 130'd12345678901234567890123456789;
    $display("c %h", c);
    $display("d %d %h", d, d);
    $display("c+d %h", c + d);
    $display("c-d %h", c - d);
    $display("c*3 %h", c * 3);
    $display("c>>70 %h c<<70 %h", c >> 70, c << 70);
    $display("cmp %b %b %b", c < d, c > d, c >= c);
    sc = -130'sd5;
    $display("sc %d %0d %h", sc, sc >>> 3, sc >>> 3);
    $display("sc/2 %0d", sc / 2);
    e = {8{64'h0123_4567_89ab_cdef}};
    f = {512{1'b1}};
    $display("e %h", e);
    $display("e+f %h", e + f);
    $display("e^f %h", e ^ f);
    $display("e>>500 %h e<<508 %h", e >> 500, e << 508);
    $display("red %b %b %b", &f, |e, ^e);
    $display("dec %d", f);
    e[300] = 1'bx;
    $display("ex %h", e);
    $display("ex %b %b", e == f, e[300:297]);
    clk = 0;
    e = {8{64'h0123_4567_89ab_cdef}};
    f = 512'd1;
    #1 $display("s %h", s);
    $display("r %h", r);
    clk = 1;
    #1 $display("r %h", r);
    $display("hier %h %b", dut.r, dut.r == r);
    f = 512'bx;
    #1 $display("s %h", s);
    for (i = 0; i < 3; i = i + 1) begin
      c = c << 1;
    end
    $display("loop %h", c);
    $finish;
  end
endmodule

// 4-state arithmetic and shifts: any unknown operand bit gives all x; shifts move states.
module xarith_tb;
  reg [7:0] a, b, d;
  reg [3:0] n;
  reg signed [7:0] s;
  integer i;
  initial begin
    a = 8'b0011x01z;
    b = 8'h0f;
    d = 8'd3;
    $display("add %b %b %b", a + b, b + d, a + 1);
    $display("sub %b %b %b", a - b, b - d, -a);
    $display("mul %b %b %b", a * 0, b * d, b * a);
    $display("div %b %b %b %b", b / 0, b % 0, b / d, b % d);
    $display("div %b %b", b / 8'bx, a % 8'd2);
    $display("pow %b %b %b", 2 ** 3, 8'bx ** 2, d ** 2);
    n = 4'bx;
    $display("shl %b %b %b", b << n, b << 2, a << 3);
    $display("shr %b %b %b", b >> n, a >> 3, a >> 8);
    s = 8'bx0000000;
    $display("ssr %b", s >>> 2);
    s = 8'bz0000000;
    $display("ssr %b", s >>> 2);
    s = 8'b1z000000;
    $display("ssr %b %b %b", s >>> 2, s <<< 1, s >> 1);
    s = -8'sd5;
    $display("sgn %0d %0d %0d %0d", s / 2, s % 2, s >>> 1, s * 3);
    i = 1'bx;
    $display("int %b %0d", i + 1, i);
    i = -7;
    $display("int %0d %0d %0d", i / 2, i % 3, i * i);
    $display("ext %b %b", s + 8'bx, {4'b0, 4'bxx01} + 1'b0);
    $finish;
  end
endmodule

// 4-state bitwise, reduction and logical operators (design note section 5).
module xlogic_tb;
  reg [7:0] a, b, c;
  reg [3:0] n;
  reg one;
  integer i;
  wire [7:0] wz;
  initial begin
    $display("start a=%b i=%b wz=%b one=%b", a, i, wz, one);
    a = 8'b0011x01z;
    b = 8'h0f;
    c = 8'bx1z0_10zx;
    $display("and %b %b %b", a & b, a & c, b & c);
    $display("or  %b %b %b", a | b, a | c, b | c);
    $display("xor %b %b %b", a ^ b, a ~^ b, a ^~ c);
    $display("not %b %b", ~a, ~c);
    $display("red %b %b %b %b %b %b", &a, |a, ^a, ~&a, ~|a, ~^a);
    $display("red %b %b %b %b", &8'b1111111x, |8'b0000000x, &8'b0x00000z, |8'b1x00000z);
    $display("red %b %b %b", ~&8'bxxxxxxx0, ~|8'bzzzzzzz1, ^8'h5a);
    $display("log %b %b %b %b", !a, !8'b0000000x, !8'b0x00010z, !8'h00);
    $display("log %b %b %b %b", a && 0, a || 1, a && 1, a || 0);
    $display("log %b %b %b", 8'bx && 8'bx, 8'bz || 8'b0, 8'b0 && 8'bx);
    one = 1'bz;
    $display("one %b %b %b %b", one, ~one, one & 1'b0, one | 1'b1);
    $display("lit %b %b %b %b", 4'bx01z, 6'bx1, 6'bz0, 8'hx5);
    $display("lit %b %b %b", 8'o7x, 12'hz_3, 'bx);
    i = 'bz;
    $display("int %b", i);
    i = 'hx5;
    $display("int %b", i);
    n = 4'b1?0?;
    $display("q %b", n);
    a = 8'bx;
    a[2] = 1'b1;
    a[6:5] = 2'b0z;
    $display("sel %b %b %b %b", a, a[2], a[6:4], a[7]);
    $finish;
  end
endmodule

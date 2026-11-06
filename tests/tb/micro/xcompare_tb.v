// == and != (known mismatch wins, else x) against === and !== (exact on four states).
module xcompare_tb;
  reg [7:0] a, b;
  reg signed [7:0] s;
  initial begin
    a = 8'b0011x01z;
    b = 8'h0f;
    $display("eq %b %b %b %b", a == b, a == 8'b1111x01z, a == 8'b0011x01z, a != 8'b1111x01z);
    $display("eq %b %b %b", a != a, 4'b1x0z == 4'b1x0z, 4'b1x0z != 4'b0x0z);
    $display("ceq %b %b %b %b", a === 8'b0011x01z, a !== 8'b0011x01z, a === 8'b0011x011, a === 8'b0011z01x);
    $display("ceq %b %b", 4'bz === 4'bz, 4'bx !== 4'bz);
    $display("rel %b %b %b %b", a < b, a <= b, a > b, a >= b);
    $display("rel %b %b %b %b", b < 8'h10, b <= 8'h0f, b > 8'h0f, b >= 8'h10);
    s = -8'sd2;
    $display("srel %b %b %b", s < 8'sd1, s > -8'sd3, s < 8'sbx);
    $display("mix %b %b", (a == b) || 1'b1, (a == 8'b0011x01z) && 1'b0);
    $finish;
  end
endmodule

// $display of x and z: %b per bit, %h/%o per digit (x, z, or X/Z for a mix), %d (x, z, X, Z) with
// the field width of the known value, %t, %c and %s.
module xfmt_tb;
  reg [7:0] a;
  reg signed [7:0] s;
  reg [15:0] w;
  integer i;
  reg [69:0] big;
  initial begin
    $display("[%b] [%h] [%o] [%d] [%0d] [%0b] [%0h] [%x]", a, a, a, a, a, a, a, a);
    a = 8'b0000x1z0;
    $display("[%b] [%h] [%o] [%d] [%0d] [%0b] [%0h] [%x]", a, a, a, a, a, a, a, a);
    a = 8'bzzzzzzzz;
    $display("[%b] [%h] [%o] [%d] [%0d] [%0o]", a, a, a, a, a, a);
    a = 8'bzzzzxxxx;
    $display("[%b] [%h] [%o] [%d] [%0d]", a, a, a, a, a);
    a = 8'b00zz0011;
    $display("[%b] [%h] [%o] [%d] [%0d] [%0b] [%0h]", a, a, a, a, a, a, a);
    a = 8'b0000000z;
    $display("[%d] [%0d] [%h] [%0h] [%0b] [%5d] [%05d]", a, a, a, a, a, a, a);
    s = 8'bx;
    $display("[%d] [%0d] [%h]", s, s, s);
    s = 8'b1000000x;
    $display("[%d] [%0d]", s, s);
    $display("[%d] [%0d] [%h]", i, i, i);
    w = 16'h12x4;
    $display("[%t] [%0t] [%h]", w, w, w);
    w = 16'h4142;
    $display("[%c] [%s]", w, w);
    big = 70'bx;
    big[3:0] = 4'b1010;
    $display("[%d] [%h] [%b]", big, big, big);
    $display("[%d]", 'bx);
    $display(a);
    $display(s, " ", a);
    $write("%b", 3'bx0z);
    $display(" w=%h", w);
    $finish;
  end
endmodule

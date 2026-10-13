// M3: output reg and output integer — variants: ANSI output reg (scalar and ranged), ANSI output integer, non-ANSI output then reg and output then integer
module m3_ansi (input a, output reg q, output reg [3:0] r, output integer n);
endmodule

module m3_nonansi (a, q, n);
  input a;
  output q;
  output n;
  reg q;
  integer n;
endmodule
module m3_top (input a, output q1, q2, output [3:0] r1, output [31:0] n1, n2);
  m3_ansi u1 (.a(a), .q(q1), .r(r1), .n(n1));
  m3_nonansi u2 (.a(a), .q(q2), .n(n2));
endmodule

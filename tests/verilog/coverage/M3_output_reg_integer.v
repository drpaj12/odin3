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


module dffsr(clk, rst, a, b, w);
  wire _00_;
  wire _01_;
  wire _02_;
  wire _03_;
  wire _04_;
  wire _05_;
  wire _06_;
  wire _07_;
  wire _08_;
  wire _09_;
  wire _10_;
  wire _11_;
  wire _12_;
  wire _13_;
  wire _14_;
  wire _15_;
  wire _16_;
  wire _17_;
  wire _18_;
  wire _19_;
  wire _20_;
  wire _21_;
  wire _22_;
  wire _23_;
  wire _24_;
  wire _25_;
  wire _26_;
  wire _27_;
  wire _28_;
  wire _29_;
  wire _30_;
  wire _31_;
  wire _32_;
  wire _33_;
  wire _34_;
  input [1:0] a;
  wire [1:0] a;
  input [1:0] b;
  wire [1:0] b;
  input clk;
  wire clk;
  input rst;
  wire rst;
  output [1:0] w;
  wire [1:0] w;
  NOT _35_ (
    .A(_05_),
    .Y(_01_)
  );
  NOT _36_ (
    .A(rst),
    .Y(_06_)
  );
  NOR2 _37_ (
    .A(_06_),
    .B(b[1]),
    .Y(_03_)
  );
  NOR2 _38_ (
    .A(_06_),
    .B(b[0]),
    .Y(_02_)
  );
  NAND2 _39_ (
    .A(rst),
    .B(b[0]),
    .Y(_04_)
  );
  NOT _40_ (
    .A(_04_),
    .Y(_00_)
  );
  NAND2 _41_ (
    .A(rst),
    .B(b[1]),
    .Y(_05_)
  );
  DFFSR _42_ (
    .C(clk),
    .D(a[0]),
    .Q(w[0]),
    .R(_02_),
    .S(_00_)
  );
  DFFSR _43_ (
    .C(clk),
    .D(a[1]),
    .Q(w[1]),
    .R(_03_),
    .S(_01_)
  );
endmodule

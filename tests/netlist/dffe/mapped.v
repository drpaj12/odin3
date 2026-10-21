
module dffe(CLK, EN, D, Q);
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
  input CLK;
  wire CLK;
  input [1:0] D;
  wire [1:0] D;
  input EN;
  wire EN;
  output [1:0] Q;
  wire [1:0] Q;
  wire pos_clk;
  NOT _35_ (
    .A(EN),
    .Y(_05_)
  );
  NAND2 _36_ (
    .A(D[0]),
    .B(EN),
    .Y(_06_)
  );
  NAND2 _37_ (
    .A(Q[0]),
    .B(_05_),
    .Y(_02_)
  );
  NAND2 _38_ (
    .A(_06_),
    .B(_02_),
    .Y(_00_)
  );
  NAND2 _39_ (
    .A(EN),
    .B(D[1]),
    .Y(_03_)
  );
  NAND2 _40_ (
    .A(_05_),
    .B(Q[1]),
    .Y(_04_)
  );
  NAND2 _41_ (
    .A(_03_),
    .B(_04_),
    .Y(_01_)
  );
  DFF _42_ (
    .C(CLK),
    .D(_00_),
    .Q(Q[0])
  );
  DFF _43_ (
    .C(CLK),
    .D(_01_),
    .Q(Q[1])
  );
  BUF _44_ (
    .A(CLK),
    .Y(pos_clk)
  );
endmodule

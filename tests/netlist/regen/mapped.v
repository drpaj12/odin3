
module regen(clk, en, d, q);
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
  wire _35_;
  wire _36_;
  wire _37_;
  wire _38_;
  wire _39_;
  wire _40_;
  wire _41_;
  wire _42_;
  wire _43_;
  wire _44_;
  wire _45_;
  wire _46_;
  wire _47_;
  wire _48_;
  wire _49_;
  wire _50_;
  wire _51_;
  wire _52_;
  wire _53_;
  wire _54_;
  wire _55_;
  wire _56_;
  wire _57_;
  wire _58_;
  wire _59_;
  wire _60_;
  wire _61_;
  wire _62_;
  input clk;
  wire clk;
  input [3:0] d;
  wire [3:0] d;
  input en;
  wire en;
  output [3:0] q;
  wire [3:0] q;
  NAND2 _63_ (
    .A(_05_),
    .B(_06_),
    .Y(_00_)
  );
  NAND2 _64_ (
    .A(en),
    .B(d[1]),
    .Y(_07_)
  );
  NAND2 _65_ (
    .A(_04_),
    .B(q[1]),
    .Y(_08_)
  );
  NAND2 _66_ (
    .A(_07_),
    .B(_08_),
    .Y(_01_)
  );
  NAND2 _67_ (
    .A(en),
    .B(d[2]),
    .Y(_09_)
  );
  NAND2 _68_ (
    .A(_04_),
    .B(q[2]),
    .Y(_10_)
  );
  NAND2 _69_ (
    .A(_09_),
    .B(_10_),
    .Y(_02_)
  );
  NAND2 _70_ (
    .A(en),
    .B(d[3]),
    .Y(_11_)
  );
  NAND2 _71_ (
    .A(_04_),
    .B(q[3]),
    .Y(_12_)
  );
  NAND2 _72_ (
    .A(_11_),
    .B(_12_),
    .Y(_03_)
  );
  NOT _73_ (
    .A(en),
    .Y(_04_)
  );
  NAND2 _74_ (
    .A(d[0]),
    .B(en),
    .Y(_05_)
  );
  NAND2 _75_ (
    .A(q[0]),
    .B(_04_),
    .Y(_06_)
  );
  DFF _76_ (
    .C(clk),
    .D(_00_),
    .Q(q[0])
  );
  DFF _77_ (
    .C(clk),
    .D(_01_),
    .Q(q[1])
  );
  DFF _78_ (
    .C(clk),
    .D(_02_),
    .Q(q[2])
  );
  DFF _79_ (
    .C(clk),
    .D(_03_),
    .Q(q[3])
  );
endmodule

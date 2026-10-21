
module cone(a, b, c, d, y0, y1);
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
  input a;
  wire a;
  input b;
  wire b;
  input c;
  wire c;
  input d;
  wire d;
  output y0;
  wire y0;
  output y1;
  wire y1;
  NAND2 _32_ (
    .A(b),
    .B(a),
    .Y(_00_)
  );
  NAND2 _33_ (
    .A(d),
    .B(_05_),
    .Y(_01_)
  );
  NAND2 _34_ (
    .A(_00_),
    .B(_01_),
    .Y(_02_)
  );
  NOR2 _35_ (
    .A(_06_),
    .B(_02_),
    .Y(_03_)
  );
  NOR2 _36_ (
    .A(_04_),
    .B(_03_),
    .Y(y0)
  );
  NAND2 _37_ (
    .A(_05_),
    .B(_02_),
    .Y(y1)
  );
  NOT _38_ (
    .A(a),
    .Y(_04_)
  );
  NOT _39_ (
    .A(c),
    .Y(_05_)
  );
  NOR2 _40_ (
    .A(d),
    .B(_05_),
    .Y(_06_)
  );
endmodule

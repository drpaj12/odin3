
module multiplexer(sel1, sel2, x_top, y_top, z_top, q_top);
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
  output [1:0] q_top;
  wire [1:0] q_top;
  input sel1;
  wire sel1;
  input sel2;
  wire sel2;
  input [1:0] x_top;
  wire [1:0] x_top;
  input [1:0] y_top;
  wire [1:0] y_top;
  input [1:0] z_top;
  wire [1:0] z_top;
  NAND2 _52_ (
    .A(_07_),
    .B(_08_),
    .Y(_09_)
  );
  NAND2 _53_ (
    .A(_01_),
    .B(_09_),
    .Y(_10_)
  );
  NAND2 _54_ (
    .A(sel1),
    .B(x_top[1]),
    .Y(_11_)
  );
  NAND2 _55_ (
    .A(_10_),
    .B(_11_),
    .Y(q_top[1])
  );
  NOT _56_ (
    .A(sel2),
    .Y(_00_)
  );
  NOT _57_ (
    .A(sel1),
    .Y(_01_)
  );
  NAND2 _58_ (
    .A(y_top[0]),
    .B(sel2),
    .Y(_02_)
  );
  NAND2 _59_ (
    .A(z_top[0]),
    .B(_00_),
    .Y(_03_)
  );
  NAND2 _60_ (
    .A(_02_),
    .B(_03_),
    .Y(_04_)
  );
  NAND2 _61_ (
    .A(_01_),
    .B(_04_),
    .Y(_05_)
  );
  NAND2 _62_ (
    .A(x_top[0]),
    .B(sel1),
    .Y(_06_)
  );
  NAND2 _63_ (
    .A(_05_),
    .B(_06_),
    .Y(q_top[0])
  );
  NAND2 _64_ (
    .A(sel2),
    .B(y_top[1]),
    .Y(_07_)
  );
  NAND2 _65_ (
    .A(_00_),
    .B(z_top[1]),
    .Y(_08_)
  );
endmodule

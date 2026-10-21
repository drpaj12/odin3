
module add(rst, x_top, y_top, q_top);
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
  output [1:0] q_top;
  wire [1:0] q_top;
  input rst;
  wire rst;
  wire [1:0] \uut.x_add2 ;
  wire [1:0] \uut.y_add2 ;
  input [1:0] x_top;
  wire [1:0] x_top;
  input [1:0] y_top;
  wire [1:0] y_top;
  NOT _56_ (
    .A(rst),
    .Y(_09_)
  );
  NAND2 _57_ (
    .A(y_top[0]),
    .B(x_top[0]),
    .Y(_10_)
  );
  NOR2 _58_ (
    .A(y_top[0]),
    .B(x_top[0]),
    .Y(_00_)
  );
  NAND2 _59_ (
    .A(_09_),
    .B(_10_),
    .Y(_01_)
  );
  NOR2 _60_ (
    .A(_00_),
    .B(_01_),
    .Y(q_top[0])
  );
  NOR2 _61_ (
    .A(y_top[1]),
    .B(x_top[1]),
    .Y(_02_)
  );
  NOT _62_ (
    .A(_02_),
    .Y(_03_)
  );
  NAND2 _63_ (
    .A(y_top[1]),
    .B(x_top[1]),
    .Y(_04_)
  );
  NAND2 _64_ (
    .A(_03_),
    .B(_04_),
    .Y(_05_)
  );
  NOR2 _65_ (
    .A(_10_),
    .B(_05_),
    .Y(_06_)
  );
  NAND2 _66_ (
    .A(_10_),
    .B(_05_),
    .Y(_07_)
  );
  NAND2 _67_ (
    .A(_09_),
    .B(_07_),
    .Y(_08_)
  );
  NOR2 _68_ (
    .A(_06_),
    .B(_08_),
    .Y(q_top[1])
  );
  BUF _69_ (
    .A(x_top[0]),
    .Y(\uut.x_add2 [0])
  );
  BUF _70_ (
    .A(x_top[1]),
    .Y(\uut.x_add2 [1])
  );
  BUF _71_ (
    .A(y_top[0]),
    .Y(\uut.y_add2 [0])
  );
  BUF _72_ (
    .A(y_top[1]),
    .Y(\uut.y_add2 [1])
  );
endmodule

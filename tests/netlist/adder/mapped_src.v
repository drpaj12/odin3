
(* top =  1  *)
(* src = "rtl.v:1.1-12.10" *)
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
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _11_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _12_;
  (* src = "../cells/cmos_map.v:4.23-4.24" *)
  wire _13_;
  (* src = "../cells/cmos_map.v:4.33-4.34" *)
  wire _14_;
  (* src = "../cells/cmos_map.v:4.23-4.24" *)
  wire _15_;
  (* src = "../cells/cmos_map.v:4.33-4.34" *)
  wire _16_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _17_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _18_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _19_;
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _20_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _21_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _22_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _23_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _24_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _25_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _26_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _27_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _28_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _29_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _30_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _31_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _32_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _33_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _34_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _35_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _36_;
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _37_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _38_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _39_;
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _40_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _41_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _42_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _43_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _44_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _45_;
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _46_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _47_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _48_;
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _49_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _50_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _51_;
  (* src = "../cells/cmos_map.v:4.23-4.24" *)
  wire _52_;
  (* src = "../cells/cmos_map.v:4.33-4.34" *)
  wire _53_;
  (* src = "../cells/cmos_map.v:4.23-4.24" *)
  wire _54_;
  (* src = "../cells/cmos_map.v:4.33-4.34" *)
  wire _55_;
  (* src = "rtl.v:5.16-5.21" *)
  output [1:0] q_top;
  wire [1:0] q_top;
  (* src = "rtl.v:2.10-2.13" *)
  input rst;
  wire rst;
  (* hdlname = "uut x_add2" *)
  (* src = "rtl.v:15.16-15.22" *)
  wire [1:0] \uut.x_add2 ;
  (* hdlname = "uut y_add2" *)
  (* src = "rtl.v:16.16-16.22" *)
  wire [1:0] \uut.y_add2 ;
  (* src = "rtl.v:3.16-3.21" *)
  input [1:0] x_top;
  wire [1:0] x_top;
  (* src = "rtl.v:4.16-4.21" *)
  input [1:0] y_top;
  wire [1:0] y_top;
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _56_ (
    .A(rst),
    .Y(_09_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _57_ (
    .A(y_top[0]),
    .B(x_top[0]),
    .Y(_10_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _58_ (
    .A(y_top[0]),
    .B(x_top[0]),
    .Y(_00_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _59_ (
    .A(_09_),
    .B(_10_),
    .Y(_01_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _60_ (
    .A(_00_),
    .B(_01_),
    .Y(q_top[0])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _61_ (
    .A(y_top[1]),
    .B(x_top[1]),
    .Y(_02_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _62_ (
    .A(_02_),
    .Y(_03_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _63_ (
    .A(y_top[1]),
    .B(x_top[1]),
    .Y(_04_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _64_ (
    .A(_03_),
    .B(_04_),
    .Y(_05_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _65_ (
    .A(_10_),
    .B(_05_),
    .Y(_06_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _66_ (
    .A(_10_),
    .B(_05_),
    .Y(_07_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _67_ (
    .A(_09_),
    .B(_07_),
    .Y(_08_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _68_ (
    .A(_06_),
    .B(_08_),
    .Y(q_top[1])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:5.9-5.41" *)
  BUF _69_ (
    .A(x_top[0]),
    .Y(\uut.x_add2 [0])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:5.9-5.41" *)
  BUF _70_ (
    .A(x_top[1]),
    .Y(\uut.x_add2 [1])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:5.9-5.41" *)
  BUF _71_ (
    .A(y_top[0]),
    .Y(\uut.y_add2 [0])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:5.9-5.41" *)
  BUF _72_ (
    .A(y_top[1]),
    .Y(\uut.y_add2 [1])
  );
endmodule

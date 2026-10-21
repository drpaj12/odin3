
(* top =  1  *)
(* src = "rtl.v:3.1-16.10" *)
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
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _12_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _13_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _14_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _15_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _16_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _17_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _18_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _19_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _20_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _21_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _22_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _23_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _24_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _25_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _26_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
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
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _37_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _38_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _39_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _40_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _41_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _42_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _43_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _44_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _45_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _46_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _47_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _48_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _49_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _50_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _51_;
  (* src = "rtl.v:9.18-9.23" *)
  output [1:0] q_top;
  wire [1:0] q_top;
  (* src = "rtl.v:4.14-4.18" *)
  input sel1;
  wire sel1;
  (* src = "rtl.v:5.14-5.18" *)
  input sel2;
  wire sel2;
  (* src = "rtl.v:6.18-6.23" *)
  input [1:0] x_top;
  wire [1:0] x_top;
  (* src = "rtl.v:7.18-7.23" *)
  input [1:0] y_top;
  wire [1:0] y_top;
  (* src = "rtl.v:8.18-8.23" *)
  input [1:0] z_top;
  wire [1:0] z_top;
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _52_ (
    .A(_07_),
    .B(_08_),
    .Y(_09_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _53_ (
    .A(_01_),
    .B(_09_),
    .Y(_10_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _54_ (
    .A(sel1),
    .B(x_top[1]),
    .Y(_11_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _55_ (
    .A(_10_),
    .B(_11_),
    .Y(q_top[1])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _56_ (
    .A(sel2),
    .Y(_00_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _57_ (
    .A(sel1),
    .Y(_01_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _58_ (
    .A(y_top[0]),
    .B(sel2),
    .Y(_02_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _59_ (
    .A(z_top[0]),
    .B(_00_),
    .Y(_03_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _60_ (
    .A(_02_),
    .B(_03_),
    .Y(_04_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _61_ (
    .A(_01_),
    .B(_04_),
    .Y(_05_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _62_ (
    .A(x_top[0]),
    .B(sel1),
    .Y(_06_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _63_ (
    .A(_05_),
    .B(_06_),
    .Y(q_top[0])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _64_ (
    .A(sel2),
    .B(y_top[1]),
    .Y(_07_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _65_ (
    .A(_00_),
    .B(z_top[1]),
    .Y(_08_)
  );
endmodule

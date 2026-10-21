
(* top =  1  *)
(* src = "rtl.v:2.1-6.10" *)
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
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _13_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _14_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _15_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _16_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _17_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.25-20.26" *)
  wire _18_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.34-20.35" *)
  wire _19_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.44-20.45" *)
  wire _20_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _21_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _22_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _23_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _24_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _25_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _26_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _27_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _28_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _29_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _30_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _31_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _32_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.25-20.26" *)
  wire _33_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.34-20.35" *)
  wire _34_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.44-20.45" *)
  wire _35_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _36_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _37_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _38_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _39_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _40_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _41_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _42_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _43_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _44_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.25-20.26" *)
  wire _45_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.34-20.35" *)
  wire _46_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.44-20.45" *)
  wire _47_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _48_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _49_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _50_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _51_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _52_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _53_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _54_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _55_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _56_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.25-20.26" *)
  wire _57_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.34-20.35" *)
  wire _58_;
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:20.44-20.45" *)
  wire _59_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _60_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _61_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _62_;
  (* src = "rtl.v:2.21-2.24" *)
  input clk;
  wire clk;
  (* src = "rtl.v:2.48-2.49" *)
  input [3:0] d;
  wire [3:0] d;
  (* src = "rtl.v:2.32-2.34" *)
  input en;
  wire en;
  (* src = "rtl.v:2.68-2.69" *)
  output [3:0] q;
  wire [3:0] q;
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _63_ (
    .A(_05_),
    .B(_06_),
    .Y(_00_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _64_ (
    .A(en),
    .B(d[1]),
    .Y(_07_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _65_ (
    .A(_04_),
    .B(q[1]),
    .Y(_08_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _66_ (
    .A(_07_),
    .B(_08_),
    .Y(_01_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _67_ (
    .A(en),
    .B(d[2]),
    .Y(_09_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _68_ (
    .A(_04_),
    .B(q[2]),
    .Y(_10_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _69_ (
    .A(_09_),
    .B(_10_),
    .Y(_02_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _70_ (
    .A(en),
    .B(d[3]),
    .Y(_11_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _71_ (
    .A(_04_),
    .B(q[3]),
    .Y(_12_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _72_ (
    .A(_11_),
    .B(_12_),
    .Y(_03_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _73_ (
    .A(en),
    .Y(_04_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _74_ (
    .A(d[0]),
    .B(en),
    .Y(_05_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _75_ (
    .A(q[0]),
    .B(_04_),
    .Y(_06_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:21.9-21.48" *)
  DFF _76_ (
    .C(clk),
    .D(_00_),
    .Q(q[0])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:21.9-21.48" *)
  DFF _77_ (
    .C(clk),
    .D(_01_),
    .Q(q[1])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:21.9-21.48" *)
  DFF _78_ (
    .C(clk),
    .D(_02_),
    .Q(q[2])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:3.5-5.20|../cells/cmos_map.v:21.9-21.48" *)
  DFF _79_ (
    .C(clk),
    .D(_03_),
    .Q(q[3])
  );
endmodule

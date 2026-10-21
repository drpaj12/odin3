
(* top =  1  *)
(* src = "rtl.v:2.1-6.10" *)
module cone(a, b, c, d, y0, y1);
  wire _00_;
  wire _01_;
  wire _02_;
  wire _03_;
  wire _04_;
  wire _05_;
  wire _06_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _07_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _08_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _09_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _10_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
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
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _18_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _19_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _20_;
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _21_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _22_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _23_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _24_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _25_;
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _26_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _27_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _28_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _29_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _30_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _31_;
  (* src = "rtl.v:2.20-2.21" *)
  input a;
  wire a;
  (* src = "rtl.v:2.29-2.30" *)
  input b;
  wire b;
  (* src = "rtl.v:2.38-2.39" *)
  input c;
  wire c;
  (* src = "rtl.v:2.47-2.48" *)
  input d;
  wire d;
  (* src = "rtl.v:2.57-2.59" *)
  output y0;
  wire y0;
  (* src = "rtl.v:2.68-2.70" *)
  output y1;
  wire y1;
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _32_ (
    .A(b),
    .B(a),
    .Y(_00_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _33_ (
    .A(d),
    .B(_05_),
    .Y(_01_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _34_ (
    .A(_00_),
    .B(_01_),
    .Y(_02_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _35_ (
    .A(_06_),
    .B(_02_),
    .Y(_03_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _36_ (
    .A(_04_),
    .B(_03_),
    .Y(y0)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _37_ (
    .A(_05_),
    .B(_02_),
    .Y(y1)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _38_ (
    .A(a),
    .Y(_04_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _39_ (
    .A(c),
    .Y(_05_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _40_ (
    .A(d),
    .B(_05_),
    .Y(_06_)
  );
endmodule

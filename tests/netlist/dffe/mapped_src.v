
(* dynports =  1  *)
(* top =  1  *)
(* src = "rtl.v:1.1-16.10" *)
module dffe(CLK, EN, D, Q);
  wire _00_;
  wire _01_;
  wire _02_;
  wire _03_;
  wire _04_;
  wire _05_;
  wire _06_;
  (* src = "../cells/cmos_map.v:4.23-4.24" *)
  wire _07_;
  (* src = "../cells/cmos_map.v:4.33-4.34" *)
  wire _08_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _09_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _10_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _11_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _12_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _13_;
  (* src = "rtl.v:12.5-14.8|../cells/cmos_map.v:20.25-20.26" *)
  wire _14_;
  (* src = "rtl.v:12.5-14.8|../cells/cmos_map.v:20.34-20.35" *)
  wire _15_;
  (* src = "rtl.v:12.5-14.8|../cells/cmos_map.v:20.44-20.45" *)
  wire _16_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _17_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _18_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
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
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _26_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _27_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _28_;
  (* src = "rtl.v:12.5-14.8|../cells/cmos_map.v:20.25-20.26" *)
  wire _29_;
  (* src = "rtl.v:12.5-14.8|../cells/cmos_map.v:20.34-20.35" *)
  wire _30_;
  (* src = "rtl.v:12.5-14.8|../cells/cmos_map.v:20.44-20.45" *)
  wire _31_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _32_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _33_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _34_;
  (* src = "rtl.v:7.11-7.14" *)
  input CLK;
  wire CLK;
  (* src = "rtl.v:8.23-8.24" *)
  input [1:0] D;
  wire [1:0] D;
  (* src = "rtl.v:7.16-7.18" *)
  input EN;
  wire EN;
  (* src = "rtl.v:9.28-9.29" *)
  output [1:0] Q;
  wire [1:0] Q;
  (* src = "rtl.v:10.10-10.17" *)
  wire pos_clk;
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _35_ (
    .A(EN),
    .Y(_05_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _36_ (
    .A(D[0]),
    .B(EN),
    .Y(_06_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _37_ (
    .A(Q[0]),
    .B(_05_),
    .Y(_02_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _38_ (
    .A(_06_),
    .B(_02_),
    .Y(_00_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _39_ (
    .A(EN),
    .B(D[1]),
    .Y(_03_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _40_ (
    .A(_05_),
    .B(Q[1]),
    .Y(_04_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _41_ (
    .A(_03_),
    .B(_04_),
    .Y(_01_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:12.5-14.8|../cells/cmos_map.v:21.9-21.48" *)
  DFF _42_ (
    .C(CLK),
    .D(_00_),
    .Q(Q[0])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:12.5-14.8|../cells/cmos_map.v:21.9-21.48" *)
  DFF _43_ (
    .C(CLK),
    .D(_01_),
    .Q(Q[1])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:5.9-5.41" *)
  BUF _44_ (
    .A(CLK),
    .Y(pos_clk)
  );
endmodule

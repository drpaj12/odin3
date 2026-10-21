
(* top =  1  *)
(* src = "rtl.v:1.1-18.10" *)
module dffsr(clk, rst, a, b, w);
  wire _00_;
  wire _01_;
  wire _02_;
  wire _03_;
  wire _04_;
  wire _05_;
  wire _06_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _07_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _08_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _09_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _10_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _11_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _12_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _13_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _14_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _15_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _16_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _17_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _18_;
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _19_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _20_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _21_;
  (* src = "../cells/cmos_map.v:16.23-16.24" *)
  wire _22_;
  (* src = "../cells/cmos_map.v:16.32-16.33" *)
  wire _23_;
  (* src = "../cells/cmos_map.v:16.42-16.43" *)
  wire _24_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.29-24.30" *)
  wire _25_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.56-24.57" *)
  wire _26_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.66-24.67" *)
  wire _27_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.47-24.48" *)
  wire _28_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.38-24.39" *)
  wire _29_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.29-24.30" *)
  wire _30_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.56-24.57" *)
  wire _31_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.66-24.67" *)
  wire _32_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.47-24.48" *)
  wire _33_;
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:24.38-24.39" *)
  wire _34_;
  (* src = "rtl.v:4.19-4.20" *)
  input [1:0] a;
  wire [1:0] a;
  (* src = "rtl.v:4.21-4.22" *)
  input [1:0] b;
  wire [1:0] b;
  (* src = "rtl.v:2.13-2.16" *)
  input clk;
  wire clk;
  (* src = "rtl.v:3.10-3.13" *)
  input rst;
  wire rst;
  (* src = "rtl.v:5.20-5.21" *)
  output [1:0] w;
  wire [1:0] w;
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _35_ (
    .A(_05_),
    .Y(_01_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _36_ (
    .A(rst),
    .Y(_06_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _37_ (
    .A(_06_),
    .B(b[1]),
    .Y(_03_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:17.10-17.49" *)
  NOR2 _38_ (
    .A(_06_),
    .B(b[0]),
    .Y(_02_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _39_ (
    .A(rst),
    .B(b[0]),
    .Y(_04_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _40_ (
    .A(_04_),
    .Y(_00_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _41_ (
    .A(rst),
    .B(b[1]),
    .Y(_05_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:25.11-25.64" *)
  DFFSR _42_ (
    .C(clk),
    .D(a[0]),
    .Q(w[0]),
    .R(_02_),
    .S(_00_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:9.2-15.5|../cells/cmos_map.v:25.11-25.64" *)
  DFFSR _43_ (
    .C(clk),
    .D(a[1]),
    .Q(w[1]),
    .R(_03_),
    .S(_01_)
  );
endmodule

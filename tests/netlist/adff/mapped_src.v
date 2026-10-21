
(* dynports =  1  *)
(* top =  1  *)
(* src = "rtl.v:1.1-21.10" *)
module adff(CLK, ARST, D, Q);
  (* src = "../cells/cmos_map.v:4.23-4.24" *)
  wire _00_;
  (* src = "../cells/cmos_map.v:4.33-4.34" *)
  wire _01_;
  (* src = "../cells/cmos_map.v:4.23-4.24" *)
  wire _02_;
  (* src = "../cells/cmos_map.v:4.33-4.34" *)
  wire _03_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.29-24.30" *)
  wire _04_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.56-24.57" *)
  wire _05_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.66-24.67" *)
  wire _06_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.47-24.48" *)
  wire _07_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.38-24.39" *)
  wire _08_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.29-24.30" *)
  wire _09_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.56-24.57" *)
  wire _10_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.66-24.67" *)
  wire _11_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.47-24.48" *)
  wire _12_;
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:24.38-24.39" *)
  wire _13_;
  (* src = "rtl.v:8.16-8.20" *)
  input ARST;
  wire ARST;
  (* src = "rtl.v:8.11-8.14" *)
  input CLK;
  wire CLK;
  (* src = "rtl.v:9.23-9.24" *)
  input [1:0] D;
  wire [1:0] D;
  (* src = "rtl.v:10.28-10.29" *)
  output [1:0] Q;
  wire [1:0] Q;
  (* src = "rtl.v:12.10-12.18" *)
  wire pos_arst;
  (* src = "rtl.v:11.10-11.17" *)
  wire pos_clk;
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:25.11-25.64" *)
  DFFSR _14_ (
    .C(CLK),
    .D(D[0]),
    .Q(Q[0]),
    .R(ARST),
    .S(1'h0)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:14.5-19.8|../cells/cmos_map.v:25.11-25.64" *)
  DFFSR _15_ (
    .C(CLK),
    .D(D[1]),
    .Q(Q[1]),
    .R(ARST),
    .S(1'h0)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:5.9-5.41" *)
  BUF _16_ (
    .A(ARST),
    .Y(pos_arst)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:5.9-5.41" *)
  BUF _17_ (
    .A(CLK),
    .Y(pos_clk)
  );
endmodule

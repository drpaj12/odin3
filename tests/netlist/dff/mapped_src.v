
(* dynports =  1  *)
(* top =  1  *)
(* src = "rtl.v:1.1-15.10" *)
module dff(CLK, D, Q);
  (* src = "../cells/cmos_map.v:4.23-4.24" *)
  wire _00_;
  (* src = "../cells/cmos_map.v:4.33-4.34" *)
  wire _01_;
  (* src = "rtl.v:11.5-13.8|../cells/cmos_map.v:20.25-20.26" *)
  wire _02_;
  (* src = "rtl.v:11.5-13.8|../cells/cmos_map.v:20.34-20.35" *)
  wire _03_;
  (* src = "rtl.v:11.5-13.8|../cells/cmos_map.v:20.44-20.45" *)
  wire _04_;
  (* src = "rtl.v:11.5-13.8|../cells/cmos_map.v:20.25-20.26" *)
  wire _05_;
  (* src = "rtl.v:11.5-13.8|../cells/cmos_map.v:20.34-20.35" *)
  wire _06_;
  (* src = "rtl.v:11.5-13.8|../cells/cmos_map.v:20.44-20.45" *)
  wire _07_;
  (* src = "rtl.v:6.11-6.14" *)
  input CLK;
  wire CLK;
  (* src = "rtl.v:7.23-7.24" *)
  input [1:0] D;
  wire [1:0] D;
  (* src = "rtl.v:8.28-8.29" *)
  output [1:0] Q;
  wire [1:0] Q;
  (* src = "rtl.v:9.10-9.17" *)
  wire pos_clk;
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:11.5-13.8|../cells/cmos_map.v:21.9-21.48" *)
  DFF _08_ (
    .C(CLK),
    .D(D[0]),
    .Q(Q[0])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:11.5-13.8|../cells/cmos_map.v:21.9-21.48" *)
  DFF _09_ (
    .C(CLK),
    .D(D[1]),
    .Q(Q[1])
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:5.9-5.41" *)
  BUF _10_ (
    .A(CLK),
    .Y(pos_clk)
  );
endmodule


(* top =  1  *)
(* src = "rtl.v:3.1-14.10" *)
module bm_DL_Dff_w_synch_reset(D, Clock, Resetn, Q);
  wire _00_;
  wire _01_;
  (* src = "../cells/cmos_map.v:12.24-12.25" *)
  wire _02_;
  (* src = "../cells/cmos_map.v:12.33-12.34" *)
  wire _03_;
  (* src = "../cells/cmos_map.v:12.43-12.44" *)
  wire _04_;
  (* src = "../cells/cmos_map.v:8.23-8.24" *)
  wire _05_;
  (* src = "../cells/cmos_map.v:8.33-8.34" *)
  wire _06_;
  (* src = "rtl.v:8.2-12.11|../cells/cmos_map.v:20.25-20.26" *)
  wire _07_;
  (* src = "rtl.v:8.2-12.11|../cells/cmos_map.v:20.34-20.35" *)
  wire _08_;
  (* src = "rtl.v:8.2-12.11|../cells/cmos_map.v:20.44-20.45" *)
  wire _09_;
  (* src = "rtl.v:4.11-4.16" *)
  input Clock;
  wire Clock;
  (* src = "rtl.v:4.8-4.9" *)
  input D;
  wire D;
  (* src = "rtl.v:5.9-5.10" *)
  output Q;
  wire Q;
  (* src = "rtl.v:4.18-4.24" *)
  input Resetn;
  wire Resetn;
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:13.11-13.50" *)
  NAND2 _10_ (
    .A(D),
    .B(Resetn),
    .Y(_01_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "../cells/cmos_map.v:9.9-9.41" *)
  NOT _11_ (
    .A(_01_),
    .Y(_00_)
  );
  (* module_not_derived = 32'd1 *)
  (* src = "rtl.v:8.2-12.11|../cells/cmos_map.v:21.9-21.48" *)
  DFF _12_ (
    .C(Clock),
    .D(_00_),
    .Q(Q)
  );
endmodule

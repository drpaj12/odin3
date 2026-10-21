
module bm_DL_Dff_w_synch_reset(D, Clock, Resetn, Q);
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
  input Clock;
  wire Clock;
  input D;
  wire D;
  output Q;
  wire Q;
  input Resetn;
  wire Resetn;
  NAND2 _10_ (
    .A(D),
    .B(Resetn),
    .Y(_01_)
  );
  NOT _11_ (
    .A(_01_),
    .Y(_00_)
  );
  DFF _12_ (
    .C(Clock),
    .D(_00_),
    .Q(Q)
  );
endmodule

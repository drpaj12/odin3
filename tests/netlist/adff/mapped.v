
module adff(CLK, ARST, D, Q);
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
  wire _13_;
  input ARST;
  wire ARST;
  input CLK;
  wire CLK;
  input [1:0] D;
  wire [1:0] D;
  output [1:0] Q;
  wire [1:0] Q;
  wire pos_arst;
  wire pos_clk;
  DFFSR _14_ (
    .C(CLK),
    .D(D[0]),
    .Q(Q[0]),
    .R(ARST),
    .S(1'h0)
  );
  DFFSR _15_ (
    .C(CLK),
    .D(D[1]),
    .Q(Q[1]),
    .R(ARST),
    .S(1'h0)
  );
  BUF _16_ (
    .A(ARST),
    .Y(pos_arst)
  );
  BUF _17_ (
    .A(CLK),
    .Y(pos_clk)
  );
endmodule

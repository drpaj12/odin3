
module dff(CLK, D, Q);
  wire _00_;
  wire _01_;
  wire _02_;
  wire _03_;
  wire _04_;
  wire _05_;
  wire _06_;
  wire _07_;
  input CLK;
  wire CLK;
  input [1:0] D;
  wire [1:0] D;
  output [1:0] Q;
  wire [1:0] Q;
  wire pos_clk;
  DFF _08_ (
    .C(CLK),
    .D(D[0]),
    .Q(Q[0])
  );
  DFF _09_ (
    .C(CLK),
    .D(D[1]),
    .Q(Q[1])
  );
  BUF _10_ (
    .A(CLK),
    .Y(pos_clk)
  );
endmodule

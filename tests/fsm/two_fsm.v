// FSM micro: two FSMs in one module (rule F-3, modules are independent of other registers).
module two_fsm(input clk, input rst, input go, output reg o, output reg p);
  reg [1:0] a;
  reg [1:0] b;
  always @(posedge clk or posedge rst)
    if (rst) a <= 2'd0;
    else case (a)
      2'd0: if (go) a <= 2'd1;
      2'd1: a <= 2'd2;
      2'd2: a <= 2'd0;
    endcase
  always @(posedge clk or posedge rst)
    if (rst) b <= 2'd0;
    else case (b)
      2'd0: if (o) b <= 2'd1;
      2'd1: b <= 2'd2;
      2'd2: b <= 2'd0;
    endcase
  always @* o = (a == 2'd2);
  always @* p = (b == 2'd1);
endmodule

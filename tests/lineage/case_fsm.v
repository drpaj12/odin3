// Lineage micro: a case that drives y (three items and a default), an independent statement for o,
// and a 3-state machine (go advances idle -> run -> done) with two outputs p and q.
module lin(input clk, input rst, input [1:0] sel, input a, input b, input c, input go,
           output reg [1:0] y, output reg o, output p, output q);
  reg [1:0] st;
  always @(posedge clk) begin
    case (sel)
      2'd0: y <= {a, b};
      2'd1: y <= {b, c};
      2'd2: y <= {a, c};
      default: y <= 2'd0;
    endcase
  end
  always @(posedge clk) o <= a ^ b ^ c;
  always @(posedge clk or posedge rst)
    if (rst) st <= 2'd0;
    else case (st)
      2'd0: if (go) st <= 2'd1;
      2'd1: st <= 2'd2;
      2'd2: if (go) st <= 2'd0;
    endcase
  assign p = st == 2'd1;
  assign q = st == 2'd2 & a;
endmodule

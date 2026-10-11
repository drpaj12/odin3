// C2: incomplete if or case (a latch) — variants: if with no else, case with no default and an unlisted value
module c2_latch (input en, d, input [1:0] s, output reg q, y);
  always @*
    if (en)
      q = d;
  always @*
    case (s)
      2'd0: y = d;
      2'd1: y = ~d;
    endcase
endmodule

// C10: full_case and parallel_case — variants: (* full_case, parallel_case *) on a case, (* parallel_case *) on a casez, // synopsys full_case parallel_case, // synopsys parallel_case, an attribute and a metacomment on one case
module c10_full_parallel (input [1:0] s, input a, b, c, output reg w, x, y, z);
  always @*
    (* full_case, parallel_case *)
    case (s)
      2'd0: w = a;
      2'd1: w = b;
      2'd2: w = c;
    endcase
  always @*
    (* parallel_case *)
    casez (s)
      2'b1?: x = a;
      2'b01: x = b;
      default: x = c;
    endcase
  always @*
    case (s) // synopsys full_case parallel_case
      2'd0: y = a;
      2'd1: y = b;
    endcase
  always @*
    (* full_case *)
    case (s) // synopsys parallel_case
      2'd0: z = a;
      2'd3: z = b;
    endcase
endmodule

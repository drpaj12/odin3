// L12: metacomments — variants: // synopsys full_case parallel_case, /* synthesis parallel_case */, // synthesis parallel_case full_case (reversed), translate_off/translate_on around comments only, a synopsys word that is not a case key
module l12_metacomments (input [1:0] s, input a, b, c, output reg x, y, z);
  always @*
    case (s) // synopsys full_case parallel_case
      2'd0: x = a;
      2'd1: x = b;
      2'd2: x = c;
    endcase
  always @*
    case (s) /* synthesis parallel_case */
      2'd0: y = a;
      default: y = b;
    endcase
  always @*
    casez (s) // synthesis parallel_case full_case
      2'b1?: z = a;
      2'b01: z = b;
      2'b00: z = c;
    endcase
  // synopsys translate_off
  // only comments inside the region
  // synopsys translate_on
  // synopsys keep
endmodule

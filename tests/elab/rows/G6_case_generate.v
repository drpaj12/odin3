// G6: case-generate — variants: several expressions per item (0, 1:), a single-item body, a named block body, a default with an unnamed block
module g6_case_gen #(parameter M = 1) (input [3:0] a, output [3:0] y);
  case (M)
    0, 1: begin : low
      assign y = a;
    end
    2: assign y = ~a;
    default: begin
      assign y = 4'd0;
    end
  endcase
endmodule

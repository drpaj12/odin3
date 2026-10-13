// G5: if-generate — variants: an else-if chain of named blocks outside a region, if/else with single items inside a region, a null else branch (;)
module g5_if_gen #(parameter W = 2) (input [3:0] a, output [3:0] y1, y2, y3);
  if (W == 1) begin : one
    assign y1 = a;
  end else if (W == 2) begin : two
    assign y1 = ~a;
  end else begin : other
    assign y1 = 4'd0;
  end
  generate
    if (W > 1)
      assign y2 = a;
    else
      assign y2 = ~a;
  endgenerate
  if (W == 2)
    assign y3 = a;
  else
    ;
endmodule

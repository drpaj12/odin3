// G2: loop generate — variants: named block inside a generate region, unnamed block outside a region, nested named loops, a single item as the body
module g2_loop_gen (input [7:0] a, output [7:0] y1, y2, y4, output [15:0] y3);
  genvar i, j;
  generate
    for (i = 0; i < 8; i = i + 1) begin : bitrev
      assign y1[i] = a[7 - i];
    end
  endgenerate
  for (i = 0; i < 4; i = i + 1) begin
    assign y2[2 * i +: 2] = a[2 * i +: 2];
  end
  for (i = 0; i < 4; i = i + 1) begin : outer
    for (j = 0; j < 4; j = j + 1) begin : inner
      assign y3[4 * i + j] = a[i] & a[j];
    end
  end
  for (i = 0; i < 8; i = i + 1)
    assign y4[i] = ~a[i];
endmodule

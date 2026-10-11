// G4: loop generate counting down — variants: from 3 down past 0 (i >= 0, step -1), from 6 down by 2
module g4_gen_down (input [7:0] a, output [3:0] y, output [7:0] z);
  genvar i;
  for (i = 3; i >= 0; i = i - 1) begin : down
    assign y[i] = a[3 - i];
  end
  for (i = 6; i >= 0; i = i - 2) begin : by_two
    assign z[i +: 2] = a[i +: 2];
  end
endmodule

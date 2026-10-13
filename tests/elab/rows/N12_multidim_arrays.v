// N12: arrays of two or more unpacked dimensions — variants: a 2-D array of vectors, a 3-D array of scalars, a read of each
module n12_multidim (input [1:0] i, j, output [7:0] y, output z);
  reg [7:0] m2 [0:3][0:1];
  reg m3 [1:0][1:0][1:0];
  assign y = m2[i][j[0]];
  assign z = m3[i[0]][j[0]][1];
endmodule

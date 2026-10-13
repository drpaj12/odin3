// N13: word select then bit or part select — variants: m[i][j], m[i][3:0], m[2][7:4], m[i][j +: 2]
module n13_word_bit_select (input [1:0] i, input [2:0] j, output y, output [3:0] p, q, output [1:0] r);
  reg [7:0] m [0:3];
  assign y = m[i][j];
  assign p = m[i][3:0];
  assign q = m[2][7:4];
  assign r = m[i][j +: 2];
endmodule

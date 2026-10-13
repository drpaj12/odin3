// Task 23: a variable-index write into a register array (the array becomes registers: written in a
// combinational block): a case over the words, after a whole assignment of each word.
module t (input [1:0] s, input [3:0] d, output [3:0] y0, y1, y2, y3);
  reg [3:0] m [0:3];
  assign y0 = m[0];
  assign y1 = m[1];
  assign y2 = m[2];
  assign y3 = m[3];
  always @* begin
    m[0] = 4'd0;
    m[1] = 4'd0;
    m[2] = 4'd0;
    m[3] = 4'd0;
    m[s] = d;
  end
endmodule

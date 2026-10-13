// N2: ascending and negative ranges — variants: vector [0:3], [1:3], [-1:2], memory [0:7] with an ascending element range, memory with a negative index range
module n2_ascending (input [0:3] a, output [1:3] y, output [-1:2] z);
  reg [0:3] mem [0:7];
  reg [3:0] neg [-2:1];
  assign y = a[1:3];
  assign z = a;
endmodule

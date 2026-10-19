// ffbits/equal_bits: two sdffe words with the same clock, enable, reset and reset value: the swapped bits merge (4 bits -> 2)
module t(input clk, input clk2, input rst, input rst2, input en, input en2, input a, input b,
         output [1:0] y, output [1:0] z);
  reg [1:0] r, s;
  always @(posedge clk) if (rst) r <= 2'b11; else if (en) r <= {a, b};
  always @(posedge clk) if (rst) s <= 2'b11; else if (en) s <= {b, a};
  assign y = r;
  assign z = s;
endmodule

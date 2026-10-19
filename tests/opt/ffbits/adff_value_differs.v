// ffbits/adff_value_differs: the async reset values differ per bit: only the equal pair merges (4 -> 3)
module t(input clk, input clk2, input rst, input rst2, input en, input en2, input a, input b,
         output [1:0] y, output [1:0] z);
  reg [1:0] r, s;
  always @(posedge clk or posedge rst) if (rst) r <= 2'b10; else r <= {a, b};
  always @(posedge clk or posedge rst) if (rst) s <= 2'b00; else s <= {b, a};
  assign y = r;
  assign z = s;
endmodule

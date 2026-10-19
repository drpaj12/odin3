// ffbits/reset_value_differs: different reset value: not merged (4 bits)
module t(input clk, input clk2, input rst, input rst2, input en, input en2, input a, input b,
         output [1:0] y, output [1:0] z);
  reg [1:0] r, s;
  always @(posedge clk) if (rst) r <= 2'b11; else if (en) r <= {a, b};
  always @(posedge clk) if (rst) s <= 2'b00; else if (en) s <= {b, a};
  assign y = r;
  assign z = s;
endmodule

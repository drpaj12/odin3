// ffbits/dffe_cascade: the second stage merges only after the first one has (8 bits -> 4)
module t(input clk, input clk2, input rst, input rst2, input en, input en2, input a, input b,
         output [1:0] y, output [1:0] z);
  reg [1:0] r, s;
  reg [1:0] r2, s2;
  always @(posedge clk) if (en) r <= {a, b};
  always @(posedge clk) if (en) s <= {b, a};
  always @(posedge clk) if (en) r2 <= r;
  always @(posedge clk) if (en) s2 <= {s[0], s[1]};
  assign y = r2;
  assign z = s2;
endmodule

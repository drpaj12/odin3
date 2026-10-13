// E13: replication — variants: {4{a}}, {2{a, b}}, nested {2{a, {2{b}}}}, a parameter count {W{a}}
module e13_replicate (input a, b, output [3:0] y1, y2, output [5:0] y3, output [1:0] y4);
  parameter W = 2;
  assign y1 = {4{a}};
  assign y2 = {2{a, b}};
  assign y3 = {2{a, {2{b}}}};
  assign y4 = {W{a}};
endmodule

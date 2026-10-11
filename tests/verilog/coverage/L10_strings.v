// L10: string literals — variants: string parameter and localparam, escapes \t \n \" \\ \101, compare ==/!= against a string, bit-select of a string parameter, string assigned to a vector
module l10_strings (input [23:0] s, output eq, ne, output [7:0] c, output [39:0] t);
  parameter NAME = "abc";
  localparam ESC = "a\tb\n\"q\"\\\101";
  assign eq = (s == "abc");
  assign ne = (NAME != "abd");
  assign c = NAME[7:0];
  assign t = "hello";
endmodule

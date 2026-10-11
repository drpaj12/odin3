// N1: wire and tri — variants: scalar wire, descending wire vector, tri vector, several names in one declaration
module n1_wire_tri (input a, input [7:0] b, output y, output [7:0] z);
  wire s;
  wire [7:0] v;
  tri [3:0] t;
  wire p, q;
  assign s = a;
  assign v = b;
  assign y = s;
  assign z = v;
endmodule

// N11: net arrays — variants: wire [3:0] w [1:0] written element by element, a scalar net array
module n11_net_arrays (input [3:0] a, b, output [3:0] y);
  wire [3:0] w [1:0];
  wire s [0:3];
  assign w[0] = a;
  assign w[1] = b;
  assign s[2] = a[0];
  assign y = w[1];
endmodule

// adder.v — behavioural model of the VTR `adder` hard block for the sim-check reference.
// lib/vtr.o3lib: `fn sumout = a ^ b ^ cin`, `fn cout = (a & b) | (a & cin) | (b & cin)`: a full
// adder, written here as a 2-bit sum so it does not share the library's expressions.
module adder(a, b, cin, cout, sumout);
  input a, b, cin;
  output cout, sumout;
  assign {cout, sumout} = a + b + cin;
endmodule

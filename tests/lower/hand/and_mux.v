// lower-sim's hand-built design (3B plan Task 2 step 1): one $and, one $mux and one $or, all
// combinational, every one with a simulate hook.
module m(input [3:0] a, input [3:0] b, input s, output [3:0] y, output [3:0] z);
  assign y = a & b;
  assign z = s ? (a | b) : ~a;
endmodule

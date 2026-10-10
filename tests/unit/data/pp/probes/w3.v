`define ID(x) x
module m(output [3:0] o); assign o = `ID(1 `ifdef X + 2 `else + 3 `endif); endmodule

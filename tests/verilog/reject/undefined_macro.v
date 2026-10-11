// L14: a macro used without a definition (the 2B preprocessor's error).
module m (output [3:0] y);
  assign y = `L14_NOT_DEFINED;
endmodule

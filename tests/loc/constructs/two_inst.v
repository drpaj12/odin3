module leaf (input a, input b, output y);
  assign y = a & b;
endmodule
module two_inst (input a, input b, input c, output y, output z);
  leaf u1 (.a(a), .b(b), .y(y));
  leaf u2 (.a(b), .b(c), .y(z));
endmodule

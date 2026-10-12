// S3: a system task as a statement (IGNORE).
module m (input a, output y);
  assign y = a;
  initial $display("a = %b", a);
endmodule

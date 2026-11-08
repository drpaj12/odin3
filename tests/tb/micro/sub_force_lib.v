module model(input clk);
  wire debug = 1'b1;
  wire cpol = 1'b1;
  reg flag;
  initial flag = 0;
  always @(posedge clk) flag <= debug;
endmodule

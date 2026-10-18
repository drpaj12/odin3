module m(input clk, input en, input set, input clr, input d, output reg q);
  always @(posedge clk or posedge set or posedge clr) if (clr) q <= 1'd0; else if (set) q <= 1'b1; else if (en) q <= d;
endmodule

module m(input clk, input en, input set, input clr, input [7:0] d, output reg [7:0] q);
  always @(posedge clk or posedge set or posedge clr) if (clr) q <= 8'd0; else if (set) q <= 8'hA5; else if (en) q <= d;
endmodule

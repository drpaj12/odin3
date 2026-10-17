module m(input clk, input set, input clr, input [3:0] d, output reg [3:0] q);
  always @(negedge clk or posedge set or posedge clr) if (clr) q <= 4'd0; else if (set) q <= 4'b1010; else q <= d;
endmodule

module m(input clk, input en, input set, input clr, input [3:0] d, output reg [3:0] q);
  always @(posedge clk or negedge set or negedge clr) if (!clr) q <= 4'd0; else if (!set) q <= 4'b1010; else if (!en) q <= d;
endmodule

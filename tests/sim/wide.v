module wide(input clk, input [69:0] d, output reg [69:0] q);
    always @(posedge clk) q <= d;
endmodule

module xff(input clk, input c, input r, input [3:0] d, output reg [3:0] q1, output reg [3:0] q2,
           output reg [3:0] q3, output reg [3:0] q4, output reg [3:0] q5);
  initial begin q1 = 4'd1; q2 = 4'd2; q3 = 4'd3; q4 = 4'd4; q5 = 4'd5; end
  always @(posedge clk) if (c) q1 <= d;
  always @(posedge clk) if (!c) q2 <= d;
  always @(posedge clk) if (c) q3 <= d; else q3 <= q3;
  always @(posedge clk) if (r) q4 <= 4'd0; else q4 <= d;
  always @(posedge clk) if (r) q5 <= 4'd7; else if (c) q5 <= d;
endmodule

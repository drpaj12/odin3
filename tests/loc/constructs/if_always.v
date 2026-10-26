module if_always (input clk, input rst, input [3:0] d, output reg [3:0] q);
  always @(posedge clk) begin
    if (rst)
      q <= 4'd0;
    else
      q <= d;
  end
endmodule

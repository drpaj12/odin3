module ord_b(input clk, input rst, output reg [3:0] q);
  initial q = 0;
  always @(posedge clk) begin
    if (rst) q <= 0;
    else q <= q + 1;
  end
endmodule

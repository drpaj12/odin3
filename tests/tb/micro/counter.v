module counter(input clk, input rst, input en, output reg [3:0] q, output tc);
  assign tc = (q == 4'd15);
  always @(posedge clk) begin
    if (rst) q <= 0;
    else if (en) q <= q + 1;
  end
endmodule

// F2: while outside a constant function (the REJECT part of a SHOULD row).
module m (input clk, output reg [3:0] y);
  integer i;
  always @(posedge clk) begin
    i = 0;
    while (i < 4) i = i + 1;
    y = i;
  end
endmodule

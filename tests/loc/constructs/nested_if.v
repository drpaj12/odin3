module nested_if (input a, input b, input c, input [1:0] x, output reg [1:0] y);
  always @(*) begin
    if (a) begin
      if (b)
        y = x;
      else
        y = 2'd1;
    end else if (c)
      y = ~x;
    else
      y = 2'd0;
  end
endmodule

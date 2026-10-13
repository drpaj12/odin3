// expr/clkinv: $dff clock driven by an inverter -> inverted clock polarity
module t(input clk, input d, output reg q);
  wire nclk = ~clk;
  always @(posedge nclk) q <= d;
endmodule

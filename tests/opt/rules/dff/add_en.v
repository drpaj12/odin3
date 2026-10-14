// dff/add_en: a $mux feeding back Q becomes a clock enable
module t(input clk, input d, input en, output reg q);
// prepass: 
  always @(posedge clk) if (en) q <= d;
endmodule

module t(input c, input [3:0] a, output [3:0] q, output reg [3:0] p);
reg [3:0] r = 4'd3;
function [3:0] f(input [3:0] x);
  begin r = x; f = x + 1; end
endfunction
always @(posedge c) p <= f(a);
assign q = r;
endmodule

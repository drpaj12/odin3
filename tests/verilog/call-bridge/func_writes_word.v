module t(input clk, input [3:0] d, input [1:0] s, output [3:0] q);
reg [3:0] m [0:3];
function [3:0] f(input [3:0] v); begin m[1] = v; f = v; end endfunction
reg [3:0] r;
always @* r = f(d);
assign q = m[s];
endmodule

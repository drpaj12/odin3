module t(input [1:0] s, input [3:0] d, output reg [3:0] q, output reg [3:0] z);
reg [3:0] m [0:3];
function [1:0] f(input [1:0] v); f = v + 1; endfunction
always @* begin
  m[0] = d; m[1] = d; m[2] = d; m[3] = d;
  m[f(s)] = ~d;
  z = d;
  q = m[s];
end
endmodule

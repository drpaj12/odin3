module t(input [3:0] d, output [3:0] q, output reg [3:0] g);
function [3:0] f(input [3:0] v);
  begin g = v + 1; f = v; end
endfunction
assign q = f(d);
endmodule

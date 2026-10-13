module t(input c, we, input [2:0] wa, ra, input [7:0] d, output [7:0] q);
reg [7:0] m [1:4];
function [7:0] f(input [7:0] x);
  begin m[wa] = x + 1; f = x + 2; end
endfunction
reg [7:0] r;
always @(posedge c) if (we) r <= f(d);
assign q = m[ra] ^ r;
endmodule

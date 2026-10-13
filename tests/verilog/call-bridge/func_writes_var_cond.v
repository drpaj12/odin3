module t(input [3:0] d, input c, output reg [3:0] q, output reg [3:0] g);
function [3:0] f(input [3:0] v);
  begin
    if (c) g = v + 4'd2;
    f = v;
  end
endfunction
always @* begin
  g = 4'd0;
  q = f(d) + 4'd1;
end
endmodule

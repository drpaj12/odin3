module t(input [3:0] d, input c, output reg [3:0] q, output reg [3:0] g, output reg [1:0] h);
function [3:0] inner(input [3:0] v);
  begin g = v + 1; inner = v; end
endfunction
function [3:0] outer(input [3:0] v);
  begin
    if (c) h[1] = 1'b1; else h[0] = v[0];
    outer = inner(v) ^ 4'd3;
  end
endfunction
always @* begin
  h = 2'b00;
  q = outer(d);
end
endmodule

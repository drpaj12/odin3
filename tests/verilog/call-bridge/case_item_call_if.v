module t(input [1:0] s, input [1:0] a, output reg [3:0] y);
function [1:0] sw(input [1:0] v);
  begin
    if (v == 2'd0) sw = 2'd3; else sw = v;
  end
endfunction
always @* begin
  case (s)
    sw(a): y = 4'd1;
    2'd2: y = 4'd2;
    default: y = 4'd9;
  endcase
end
endmodule

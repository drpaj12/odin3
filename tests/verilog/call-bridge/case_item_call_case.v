module t(input [1:0] s, input [1:0] a, b, output reg [3:0] y);
function [1:0] sw(input [1:0] v);
  begin
    case (v)
      2'd0: sw = 2'd3;
      2'd1: sw = 2'd2;
      default: sw = v;
    endcase
  end
endfunction
always @* begin
  case (s)
    sw(a): y = 4'd1;
    sw(b): y = 4'd2;
    2'd3: y = 4'd3;
    default: y = 4'd9;
  endcase
end
endmodule

// K4: function locals and control flow — variants: local reg and integer, for, if/else, case
module k4_func_locals (input [7:0] a, input [1:0] s, output [3:0] y);
  function [3:0] count_ones (input [7:0] x, input [1:0] mode);
    reg [3:0] c;
    integer i;
    begin
      c = 4'd0;
      for (i = 0; i < 8; i = i + 1)
        if (x[i])
          c = c + 1;
        else
          c = c;
      case (mode)
        2'd0: count_ones = c;
        2'd1: count_ones = ~c;
        default: count_ones = 4'd0;
      endcase
    end
  endfunction
  assign y = count_ones(a, s);
endmodule

module tb_err;
  function [3:0] inc(input [3:0] x);
    inc = x + 1;
  endfunction
  initial $display("%0d", inc(4'd1));
endmodule

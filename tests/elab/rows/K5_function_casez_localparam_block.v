// K5: casez, localparam and block declarations inside a function — variants: a localparam in the function, casez, a named block with its own reg
module k5_func_extra (input [3:0] r, output [1:0] y);
  function [1:0] prio (input [3:0] x);
    localparam NONE = 2'd0;
    begin : body
      reg [1:0] p;
      casez (x)
        4'b1???: p = 2'd3;
        4'b01??: p = 2'd2;
        4'b001?: p = 2'd1;
        default: p = NONE;
      endcase
      prio = p;
    end
  endfunction
  assign y = prio(r);
endmodule

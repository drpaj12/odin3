// P4: a real parameter (the REJECT part of a MUST row).
module m (output [7:0] y);
  parameter real R = 1;
  assign y = 8'd0;
endmodule

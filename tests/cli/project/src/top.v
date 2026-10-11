`include "defs.vh"
module top (input wire [`WIDTH-1:0] a, output wire [`WIDTH-1:0] y);
    assign y = `INV(a);
endmodule

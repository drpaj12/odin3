`include "w.vh"
module a (input wire [`MSB:0] x, output wire [`MSB:0] y);
    b u_b (.x(x[3:0]), .y(y[3:0]));
    assign y[`MSB:4] = x[`MSB:4];
endmodule

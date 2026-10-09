// sat4 is not listed: it is found in the USER_LIBRARIES directory lib/ as lib/sat4.v.
`include "defs.vh"

module top (
    input  wire [3:0] a,
    input  wire [3:0] b,
    output wire [3:0] y
);
    sat4 u_sat (.a(a), .b(b), .y(y));
endmodule

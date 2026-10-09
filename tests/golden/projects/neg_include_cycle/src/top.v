// Includes a.vh, which includes b.vh, which includes a.vh again.
`include "a.vh"

module top (
    input  wire a,
    output wire y
);
    assign y = a;
endmodule

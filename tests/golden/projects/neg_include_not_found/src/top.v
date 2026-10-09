// Includes a file that is nowhere: not next to this file, not on the include path.
`include "missing_defs.vh"

module top (
    input  wire a,
    output wire y
);
    assign y = a;
endmodule

// op.vh exists in incA and incB: the first directory on the include path wins (AND).
// With incB first the netlist would be an OR.
`include "op.vh"

module top (
    input  wire [3:0] a,
    input  wire [3:0] b,
    output wire [3:0] y
);
    assign y = `OP(a, b);
endmodule

// Registered ALU; opcodes and width come from alu_pkg (wildcard import).
import alu_pkg::*;

module alu (
    input  logic         clk,
    input  logic [1:0]   op,
    input  logic [W-1:0] a,
    input  logic [W-1:0] b,
    output logic [W-1:0] y
);
    always_ff @(posedge clk)
        case (op)
            OP_ADD:  y <= a + b;
            OP_SUB:  y <= a - b;
            OP_AND:  y <= a & b;
            default: y <= a | b;
        endcase
endmodule

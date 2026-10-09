// Shared ALU definitions: operand width and opcodes.
package alu_pkg;
    localparam int W = 8;
    localparam logic [1:0] OP_ADD = 2'd0;
    localparam logic [1:0] OP_SUB = 2'd1;
    localparam logic [1:0] OP_AND = 2'd2;
    localparam logic [1:0] OP_OR  = 2'd3;
endpackage

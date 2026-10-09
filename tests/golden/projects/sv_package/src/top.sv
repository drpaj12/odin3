// Uses the package by qualified name (alu_pkg::W) and instantiates alu.
module top (
    input  logic                  clk,
    input  logic [1:0]            op,
    input  logic [alu_pkg::W-1:0] a,
    input  logic [alu_pkg::W-1:0] b,
    output logic [alu_pkg::W-1:0] y
);
    alu u_alu (.clk(clk), .op(op), .a(a), .b(b), .y(y));
endmodule

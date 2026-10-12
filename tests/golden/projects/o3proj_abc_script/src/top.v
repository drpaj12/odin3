// Soft logic for the project's ABC script: a 4-bit compare and select (stored, not applied).
module top (
    input  wire [3:0] a,
    input  wire [3:0] b,
    input  wire       sel,
    output wire [3:0] y,
    output wire       eq
);
    assign y  = sel ? a & b : a ^ b;
    assign eq = a == b;
endmodule

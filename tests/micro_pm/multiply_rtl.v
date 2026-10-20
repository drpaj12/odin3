// The RTL of tests/micro/verilog/micro/multiply_hard_block.v with `*`: the reference for the soft
// `multiply` (D18).
module top_module (
    input [1:0] x1, x2, x3, x4,
    output [3:0] y1, y2
);
    assign y1 = x1 * x2;
    assign y2 = x3 * x4;
endmodule

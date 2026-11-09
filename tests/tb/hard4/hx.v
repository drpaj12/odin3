// A partly unknown operand into an adder: the carry out of the unknown low bits is 0 whatever
// they are, so the top bits are known. A hard adder chain must say so as the lowered gates do.
module hx(input clk, input ld, input [1:0] d, output [3:0] y, output [3:0] z);
    reg [1:0] r;
    always @(posedge clk) if (ld) r <= d;
    assign y = {2'b00, r} + 4'd4;
    assign z = y - 4'd1;
endmodule

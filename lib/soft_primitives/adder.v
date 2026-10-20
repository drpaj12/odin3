// Soft body of the VTR primitive `adder` (Odin III's own behavioural description, MIT). Read only
// when the architecture record has no `adder` model (4.2 D18).
module adder #(
    parameter WIDTH = 1
) (
    input [WIDTH-1:0] a,
    input [WIDTH-1:0] b,
    input cin,
    output cout,
    output [WIDTH-1:0] sumout
);
    assign {cout, sumout} = {1'b0, a} + {1'b0, b} + cin;
endmodule

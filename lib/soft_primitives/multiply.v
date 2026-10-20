// Soft body of the VTR primitive `multiply` (Odin III's own behavioural description, MIT). Read
// only when the architecture record has no `multiply` model (4.2 D18).
module multiply #(
    parameter WIDTH = 1
) (
    input [WIDTH-1:0] a,
    input [WIDTH-1:0] b,
    output [2*WIDTH-1:0] out
);
    assign out = a * b;
endmodule

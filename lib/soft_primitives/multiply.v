// Soft body of the VTR primitive `multiply` (Odin III's own behavioural description, MIT). Read
// only when the architecture record has no `multiply` model, or an instance's `#()` does not fit
// the hard cell of that name (4.2 D18, #262).
module multiply #(
    parameter WIDTH = 1
) (
    input [WIDTH-1:0] a,
    input [WIDTH-1:0] b,
    output [2*WIDTH-1:0] out
);
    assign out = a * b;
endmodule

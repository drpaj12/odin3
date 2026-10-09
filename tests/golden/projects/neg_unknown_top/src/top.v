// Defines top; the project names a top that does not exist.
module top (
    input  wire a,
    output wire y
);
    assign y = ~a;
endmodule

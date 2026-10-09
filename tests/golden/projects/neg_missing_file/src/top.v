// Present; the project also names src/missing.v, which does not exist.
module top (
    input  wire a,
    output wire y
);
    assign y = ~a;
endmodule

// Any design: the .qip files include each other.
module top (
    input  wire a,
    output wire y
);
    assign y = ~a;
endmodule

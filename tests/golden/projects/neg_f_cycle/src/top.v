// Any design: the file lists include each other.
module top (
    input  wire a,
    output wire y
);
    assign y = ~a;
endmodule

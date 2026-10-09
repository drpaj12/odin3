// -y cell nobody instantiates: a library directory is searched, not read whole.
module or2 (
    input  wire a,
    input  wire b,
    output wire y
);
    assign y = a | b;
endmodule

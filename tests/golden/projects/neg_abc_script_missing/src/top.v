// Any design: the project names an ABC script that does not exist.
module top (
    input  wire a,
    output wire y
);
    assign y = ~a;
endmodule

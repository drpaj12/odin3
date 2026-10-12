// Any design: the project names an ABC script in a -f list, which has no form for it.
module top (
    input  wire a,
    output wire y
);
    assign y = ~a;
endmodule

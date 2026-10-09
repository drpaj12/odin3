// Any design: the list uses a variable that is not set.
module top (
    input  wire a,
    output wire y
);
    assign y = ~a;
endmodule

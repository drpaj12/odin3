// Instantiates a module no file defines.
module top (
    input  wire a,
    output wire y
);
    missing_cell u_cell (.a(a), .y(y));
endmodule

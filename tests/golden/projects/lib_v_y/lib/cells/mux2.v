// -y cell: built from inv, which comes from the -v file.
module mux2 (
    input  wire a,
    input  wire b,
    input  wire s,
    output wire y
);
    wire sn;

    inv u_inv (.a(s), .y(sn));
    assign y = (a & sn) | (b & s);
endmodule

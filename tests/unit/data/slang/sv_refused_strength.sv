// Fixture: an assignment drive strength is refused with a located error, never silently dropped.
module refused_strength (
    input  logic a,
    input  logic b,
    output logic y
);
    assign (strong1, pull0) y = a & b;
endmodule

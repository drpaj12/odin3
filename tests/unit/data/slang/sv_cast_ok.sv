// Fixture: an explicit width cast is accepted (truncation is a part select).
module cast_ok (
    input  logic [7:0] a,
    input  logic [3:0] b,
    output logic       y
);
    assign y = (4'(a) == b);
endmodule

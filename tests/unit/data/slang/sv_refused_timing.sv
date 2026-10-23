// Fixture: an intra-assignment delay is refused with a located error, never silently dropped.
module refused_timing (
    input  logic       clk,
    input  logic       d,
    output logic       q
);
    always_ff @(posedge clk) q <= #1 d;
endmodule

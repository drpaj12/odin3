// Fixture: `priority if` is refused with a located error, never silently dropped.
module refused_priority (
    input  logic a,
    output logic y
);
    always_comb begin
        priority if (a) y = 1'b1;
        else y = 1'b0;
    end
endmodule

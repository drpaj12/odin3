// Fixture: `priority if` is accepted; the qualifier is dropped.
module priority_ok (
    input  logic a,
    output logic y
);
    always_comb begin
        priority if (a) y = 1'b1;
        else y = 1'b0;
    end
endmodule

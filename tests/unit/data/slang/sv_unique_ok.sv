// Fixture: `unique case` is accepted; the qualifier is dropped.
module unique_ok (
    input  logic [1:0] sel,
    output logic       y
);
    always_comb begin
        unique case (sel)
            2'd0: y = 1'b0;
            default: y = 1'b1;
        endcase
    end
endmodule

// Fixture: a `ref` port is refused with a located error, never mapped to an inout.
module refused_ref (
    input  logic clk,
    ref    logic x
);
    always_ff @(posedge clk) x <= ~x;
endmodule

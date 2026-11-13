// Fixture for the explicit-cast review finding: the Odin AST has no cast node, so the adapter
// must refuse `4'(a)` with a located error rather than silently compare `a` untruncated.
module bad_cast (
    input  logic [7:0] a,
    input  logic [3:0] b,
    output logic       y
);
    assign y = (4'(a) == b);
endmodule

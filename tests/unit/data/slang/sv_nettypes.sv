// Fixture: tri/wand/wor net types keep their kind in the NET_DECL (review Low), so the
// multi-driver semantics are not silently turned into a wire.
module nettypes (
    input  wire a,
    input  wire b,
    output wire y,
    output wire z
);
    wand w;
    wor  v;

    assign w = a;
    assign v = b;
    assign y = w;
    assign z = v;
endmodule

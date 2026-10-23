// Fixture for the signed declarations review finding: a `wire signed` NET_DECL and a
// `logic signed` VAR_DECL of an always_comb must keep SIGNED, or `s < 0` / `v < 0` fold to gnd.
module signed_cmp (
    input  wire signed [3:0] a,
    input  wire signed [3:0] b,
    output wire              lt,
    output wire              alt
);
    wire  signed [3:0] s;
    logic signed [3:0] v;

    assign s = a;
    always_comb v = b;
    assign lt = s < 0;
    assign alt = v < 0;
endmodule

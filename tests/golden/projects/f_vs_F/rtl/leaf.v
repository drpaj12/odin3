// rtl/leaf.v: named by lists/a.f, read with -f (relative to the outermost list).
module leaf_root (
    input  wire [3:0] a,
    output wire [3:0] y
);
    assign y = ~a;
endmodule

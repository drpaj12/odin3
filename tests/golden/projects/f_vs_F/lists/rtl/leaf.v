// lists/rtl/leaf.v: named by lists/b.f, read with -F (relative to that list).
module leaf_lists (
    input  wire [3:0] a,
    output wire [3:0] y
);
    assign y = a + 4'd1;
endmodule

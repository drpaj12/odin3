// Uses one module from each leaf.v.
module top (
    input  wire [3:0] a,
    output wire [3:0] y
);
    wire [3:0] t;

    leaf_root u_root (.a(a), .y(t));
    leaf_lists u_lists (.a(t), .y(y));
endmodule

// a_blk then b_blk.
module top (
    input  wire [3:0] a,
    output wire [3:0] y
);
    wire [3:0] t;

    a_blk u_a (.a(a), .y(t));
    b_blk u_b (.a(t), .y(y));
endmodule

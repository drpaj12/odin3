// Shared by a_blk and b_blk; listed twice through a diamond of file lists, read once.
module shared (
    input  wire [3:0] a,
    output wire [3:0] y
);
    assign y = {a[0], a[3:1]};
endmodule

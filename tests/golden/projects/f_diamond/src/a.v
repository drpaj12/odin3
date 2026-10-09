// Uses shared.
module a_blk (
    input  wire [3:0] a,
    output wire [3:0] y
);
    shared u_s (.a(a), .y(y));
endmodule

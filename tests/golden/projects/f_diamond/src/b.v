// Uses shared too.
module b_blk (
    input  wire [3:0] a,
    output wire [3:0] y
);
    wire [3:0] t;

    shared u_s (.a(a), .y(t));
    assign y = ~t;
endmodule

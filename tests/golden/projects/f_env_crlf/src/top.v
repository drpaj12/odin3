// Top, named by -top.
module top (
    input  wire [3:0] a,
    output wire [3:0] y
);
    leaf u_leaf (.a(a), .y(y));
endmodule

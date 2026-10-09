// Two majority stages.
module mid (
    input  wire [5:0] x,
    output wire [1:0] y
);
    wire [1:0] t;

    leaf u_first (.a(x[1:0]), .b(x[3:2]), .c(x[5:4]), .y(t));
    leaf u_second (.a(t), .b(x[1:0]), .c(x[5:4]), .y(y));
endmodule

// The project overrides W of instance u_wide only (u_narrow keeps W = 2).
module top (
    input  wire [5:0] a,
    output wire [5:0] y,
    output wire [1:0] z
);
    inv #(.W(2)) u_narrow (.a(a[1:0]), .y(z));
    sub u_wide (.a(a), .y(y));
endmodule

// sub wraps inv; its own W (default 2) is what the project overrides on u_wide.
module sub #(parameter W = 2) (
    input  wire [5:0] a,
    output wire [5:0] y
);
    inv #(.W(W)) u_inv (.a(a[W-1:0]), .y(y[W-1:0]));
    assign y[5:W] = a[5:W];
endmodule
